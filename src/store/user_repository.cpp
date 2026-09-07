#include "store/user_repository.h"

#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <utility>

#include "common/json_utils.h"

namespace http_server_demo {

namespace {

constexpr int kUserCacheTtl = 60;                            // 单用户缓存 60s
constexpr int kListCacheTtl = 10;                            // 列表缓存 10s
constexpr int kStatsCacheTtl = 30;                           // 统计缓存 30s
constexpr const char* kListCacheIndex = "cache:users:list";  // 列表缓存 key 索引

int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string ListCacheKey(int limit, int offset) {
  return "users:list:" + std::to_string(limit) + ":" + std::to_string(offset);
}

}  // namespace

UserRepository::UserRepository(std::shared_ptr<MysqlPool> pool, std::shared_ptr<RedisClient> redis)
    : pool_(std::move(pool)), redis_(std::move(redis)) {}

void UserRepository::CountRequest() { redis_->incr("stats:request_count"); }

void UserRepository::InvalidateListCache() {
  // 删除所有已记录的列表缓存 key，再清掉索引本身
  auto keys = redis_->smembers(kListCacheIndex);
  if (keys) {
    for (const auto& k : *keys) {
      redis_->del(k);
    }
  }
  redis_->del(kListCacheIndex);
}

bool UserRepository::CreateUser(const std::string& name, const std::string& email,
                                user::v1::User* out) {
  int64_t now = NowUnixSeconds();

  auto guard = pool_->acquire();
  MysqlConn* conn = guard.get();

  std::string sql = "INSERT INTO users(name, email, created_at) VALUES('" + conn->escape(name) +
                    "','" + conn->escape(email) + "'," + std::to_string(now) + ")";
  if (!conn->execute(sql)) return false;

  user::v1::User user;
  user.set_id(std::to_string(conn->lastInsertId()));
  user.set_name(name);
  user.set_email(email);
  user.set_created_at(now);

  // 写缓存 + 失效列表/统计缓存
  redis_->set("user:" + user.id(), UserToJsonString(user), kUserCacheTtl);
  InvalidateListCache();
  redis_->del("stats:total_users");

  *out = std::move(user);
  return true;
}

std::optional<user::v1::User> UserRepository::GetUser(const std::string& id) {
  // 1. 先查 Redis 缓存
  auto cached = redis_->get("user:" + id);
  if (cached) {
    try {
      user::v1::User u;
      JsonToUser(nlohmann::json::parse(*cached), &u);
      return u;
    } catch (const std::exception&) {
      // 缓存损坏，回源数据库
    }
  }

  // 2. 未命中，查 MySQL
  auto guard = pool_->acquire();
  MysqlConn* conn = guard.get();
  MysqlResult res(conn->query("SELECT id, name, email, created_at FROM users WHERE id='" +
                              conn->escape(id) + "'"));
  if (!res) return std::nullopt;

  MYSQL_ROW row = mysql_fetch_row(res.get());
  if (!row) return std::nullopt;

  user::v1::User u;
  u.set_id(row[0]);
  u.set_name(row[1]);
  u.set_email(row[2]);
  u.set_created_at(std::strtoll(row[3], nullptr, 10));

  // 3. 回填缓存
  redis_->set("user:" + id, UserToJsonString(u), kUserCacheTtl);
  return u;
}

bool UserRepository::ListUsers(int limit, int offset, std::vector<user::v1::User>* out,
                               int64_t* total) {
  std::string cacheKey = ListCacheKey(limit, offset);

  // 1. 查缓存
  auto cached = redis_->get(cacheKey);
  if (cached) {
    try {
      auto j = nlohmann::json::parse(*cached);
      out->clear();
      for (const auto& ju : j["users"]) {
        user::v1::User u;
        JsonToUser(ju, &u);
        out->push_back(std::move(u));
      }
      *total = j.value("total", static_cast<int64_t>(0));
      return true;
    } catch (const std::exception&) {
      // 缓存损坏，回源
    }
  }

  // 2. 查 MySQL
  auto guard = pool_->acquire();
  MysqlConn* conn = guard.get();

  // 2a. 总数
  MysqlResult resCount(conn->query("SELECT COUNT(*) FROM users"));
  if (!resCount) return false;
  MYSQL_ROW countRow = mysql_fetch_row(resCount.get());
  int64_t totalCount = countRow ? std::strtoll(countRow[0], nullptr, 10) : 0;

  // 2b. 分页数据
  std::string sql = "SELECT id, name, email, created_at FROM users ORDER BY id LIMIT " +
                    std::to_string(limit) + " OFFSET " + std::to_string(offset);
  MysqlResult res(conn->query(sql));
  if (!res) return false;

  std::vector<user::v1::User> users;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res.get()))) {
    user::v1::User u;
    u.set_id(row[0]);
    u.set_name(row[1]);
    u.set_email(row[2]);
    u.set_created_at(std::strtoll(row[3], nullptr, 10));
    users.push_back(std::move(u));
  }

  // 3. 写缓存（并把 key 记入索引，供写时批量失效）
  nlohmann::json j;
  j["total"] = totalCount;
  j["users"] = nlohmann::json::array();
  for (const auto& u : users) {
    j["users"].push_back(UserToJson(u));
  }
  redis_->set(cacheKey, j.dump(), kListCacheTtl);
  redis_->sadd(kListCacheIndex, cacheKey);

  *out = std::move(users);
  *total = totalCount;
  return true;
}

bool UserRepository::GetStats(int64_t* total_users, int64_t* request_count) {
  // total_users：缓存优先
  auto cached = redis_->get("stats:total_users");
  if (cached) {
    *total_users = std::strtoll(cached->c_str(), nullptr, 10);
  } else {
    auto guard = pool_->acquire();
    MysqlConn* conn = guard.get();
    MysqlResult res(conn->query("SELECT COUNT(*) FROM users"));
    if (!res) return false;
    MYSQL_ROW row = mysql_fetch_row(res.get());
    int64_t total = row ? std::strtoll(row[0], nullptr, 10) : 0;
    redis_->set("stats:total_users", std::to_string(total), kStatsCacheTtl);
    *total_users = total;
  }

  // request_count：Redis INCR 计数
  auto rc = redis_->get("stats:request_count");
  *request_count = rc ? std::strtoll(rc->c_str(), nullptr, 10) : 0;
  return true;
}

}  // namespace http_server_demo
