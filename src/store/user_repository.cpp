// user_repository.cpp —— 用户数据访问层的实现（Cache-Aside 模式的具体编排）
//
// 阅读建议先看 .h 的类注释（读/写两条路径的决策），再看这里每个方法：
//   CreateUser  → 写路径示范（先 DB 后缓存/失效）
//   GetUser     → 读路径示范（先缓存后回源回填）—— 本类最核心的模板
//   ListUsers   → 带列表缓存的读 + 缓存 key 索引
//   GetStats    → 部分走缓存、部分走计数
#include "store/user_repository.h"

#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <utility>

#include "common/json_utils.h"

namespace http_server_demo {

namespace {

// ---------------- 缓存 TTL 策略 ----------------
// TTL = 缓存的"兜底过期时间"。即使写时失效遗漏，到期后缓存也会自动消失，
// 下次读重新回源 —— 这就是"写时失效 + 短 TTL"双保险的一致性方案。
constexpr int kUserCacheTtl = 60;   // 单用户缓存 60s
constexpr int kListCacheTtl = 10;   // 列表缓存 10s（变化频繁，设短些）
constexpr int kStatsCacheTtl = 30;  // 统计缓存 30s
// 记录"已产生的列表缓存 key"的 Redis Set 键名（作用见 ListUsers/InvalidateListCache）
constexpr const char* kListCacheIndex = "cache:users:list";

// 当前 Unix 时间戳（秒），写入 created_at
int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 列表缓存的键：把分页参数拼进 key —— 不同页(limit/offset)结果不同，
// 若共用同一个 key 会互相覆盖错乱，所以按参数区分。
std::string ListCacheKey(int limit, int offset) {
  return "users:list:" + std::to_string(limit) + ":" + std::to_string(offset);
}

}  // namespace

// 构造：接收外部注入的 MySQL 连接池与 Redis 客户端并接管（shared_ptr 共享所有权）
UserRepository::UserRepository(std::shared_ptr<MysqlPool> pool, std::shared_ptr<RedisClient> redis)
    : pool_(std::move(pool)), redis_(std::move(redis)) {}

// 请求计数：直接把 Redis 的 stats:request_count INCR +1。
// 用 Redis 而非 MySQL 是因为计数是高频写，走内存原子操作更合适，
// 且该数据允许丢失（可重建），不占持久层。
void UserRepository::CountRequest() { redis_->incr("stats:request_count"); }

// 失效所有列表缓存（新增用户后调用）：
//   1. 从索引 Set(kListCacheIndex) 取出历史上写过的每个列表缓存 key；
//   2. 逐个 DEL 删除 —— Redis 不支持通配符 DEL，所以必须靠这个索引枚举；
//   3. 最后把索引 Set 本身也删掉（下次 ListUsers 重建索引）。
// 这是"写时失效"保证列表一致性的关键：任何分页都可能因新用户而过期。
void UserRepository::InvalidateListCache() {
  auto keys = redis_->smembers(kListCacheIndex);
  if (keys) {
    for (const auto& k : *keys) {
      redis_->del(k);
    }
  }
  redis_->del(kListCacheIndex);
}

// ============================== 写路径 ==============================
// 创建用户的顺序设计（Cache-Aside 写路径）：
//   1) 先 INSERT MySQL —— 数据落盘，数据库成为唯一真源；
//   2) 再"写缓存 / 失效缓存"—— 让缓存尽快反映新数据。
// 为什么顺序不能反过来？如果先写缓存、写 MySQL 才失败，缓存里会出现
// "数据库里没有的假用户"。先写 DB，即使后续缓存操作失败，也只是读到旧缓存
// （TTL 会兜底过期），不会出现数据错乱。
bool UserRepository::CreateUser(const std::string& name, const std::string& email,
                                user::v1::User* out) {
  int64_t now = NowUnixSeconds();  // 记录创建时间（Unix 秒）

  // 从连接池借一条 MySQL 连接（Guard 析构自动归还，异常安全）
  auto guard = pool_->acquire();
  MysqlConn* conn = guard.get();

  // 拼 INSERT 语句。所有用户可控字段（name/email）都过 conn->escape() 转义，
  // 防止 SQL 注入；数值型字段(now)由我们自己生成，无需转义。
  std::string sql = "INSERT INTO users(name, email, created_at) VALUES('" + conn->escape(name) +
                    "','" + conn->escape(email) + "'," + std::to_string(now) + ")";
  if (!conn->execute(sql)) return false;  // 写库失败：直接失败，不动缓存

  // 组装业务对象：id 用数据库自增主键（AUTO_INCREMENT 由 lastInsertId() 取回）
  user::v1::User user;
  user.set_id(std::to_string(conn->lastInsertId()));
  user.set_name(name);
  user.set_email(email);
  user.set_created_at(now);

  // ---- 缓存维护（写路径第二步）----
  // 1) 主动把新用户写进单用户缓存（key=user:{id}），下次 GetUser 直接命中；
  redis_->set("user:" + user.id(), UserToJsonString(user), kUserCacheTtl);
  // 2) 列表缓存整体失效：任何分页里现在都可能漏掉这个新用户，全删最省心；
  InvalidateListCache();
  // 3) 用户总数缓存失效：总数已 +1，删掉让下次 GetStats 回源重算。
  redis_->del("stats:total_users");

  *out = std::move(user);
  return true;
}

// ============================== 读路径 ==============================
// 这是 Cache-Aside 读路径的"标准模板"，其余读方法都遵循同一套路：
//   缓存命中 → 直接返回（不碰 DB，性能关键）；
//   未命中   → 查 MySQL → 回填缓存（带 TTL）→ 返回。
// 好处：同一数据首次读打 DB，之后都打缓存，热点数据压力被 Redis 挡掉。
std::optional<user::v1::User> UserRepository::GetUser(const std::string& id) {
  // ---- 第 1 步：查缓存 ----
  auto cached = redis_->get("user:" + id);
  if (cached) {
    try {
      // 缓存里存的是 JSON 字符串，反序列化回 proto User
      user::v1::User u;
      JsonToUser(nlohmann::json::parse(*cached), &u);
      return u;  // 命中：直接返回
    } catch (const std::exception&) {
      // 缓存里的 JSON 损坏（理论上不该发生）：忽略，继续回源数据库
    }
  }

  // ---- 第 2 步：未命中，查 MySQL ----
  auto guard = pool_->acquire();  // 借连接
  MysqlConn* conn = guard.get();
  // 按主键查询；id 同样 escape 防注入
  MysqlResult res(conn->query("SELECT id, name, email, created_at FROM users WHERE id='" +
                              conn->escape(id) + "'"));
  if (!res) return std::nullopt;  // SQL 执行出错

  MYSQL_ROW row = mysql_fetch_row(res.get());  // 取第一行
  if (!row) return std::nullopt;               // 没有匹配行 = 用户不存在

  // 把一行（row[i] 都是 const char*）转成 proto User
  user::v1::User u;
  u.set_id(row[0]);
  u.set_name(row[1]);
  u.set_email(row[2]);
  u.set_created_at(std::strtoll(row[3], nullptr, 10));  // 字符串 → int64

  // ---- 第 3 步：回填缓存 ----
  // 把刚从 DB 查到的数据写回 Redis 并设 TTL，下次同 id 的读直接命中缓存
  redis_->set("user:" + id, UserToJsonString(u), kUserCacheTtl);
  return u;
}

// 分页列出用户（读路径 + 列表缓存）。
// 与 GetUser 的单条缓存相比，这里多处理两个问题：
//   a) key 要区分分页参数（不同 limit/offset 是不同结果）；
//   b) "新增用户"会让所有分页过期，需要把 key 记入索引 Set 供写时批量失效。
bool UserRepository::ListUsers(int limit, int offset, std::vector<user::v1::User>* out,
                               int64_t* total) {
  std::string cacheKey = ListCacheKey(limit, offset);  // users:list:{limit}:{offset}

  // ---- 第 1 步：查缓存（整份列表 + total 一起缓存）----
  auto cached = redis_->get(cacheKey);
  if (cached) {
    try {
      // 缓存结构：{"total": N, "users":[{...}, {...}]}
      auto j = nlohmann::json::parse(*cached);
      out->clear();
      for (const auto& ju : j["users"]) {  // 逐个反序列化为 proto User
        user::v1::User u;
        JsonToUser(ju, &u);
        out->push_back(std::move(u));
      }
      *total = j.value("total", static_cast<int64_t>(0));
      return true;  // 命中：直接返回
    } catch (const std::exception&) {
      // 缓存损坏：忽略并回源
    }
  }

  // ---- 第 2 步：未命中，查 MySQL（需要借一条连接）----
  auto guard = pool_->acquire();
  MysqlConn* conn = guard.get();

  // 2a. 先查总数（COUNT 聚合，结果单行单列）
  MysqlResult resCount(conn->query("SELECT COUNT(*) FROM users"));
  if (!resCount) return false;
  MYSQL_ROW countRow = mysql_fetch_row(resCount.get());
  int64_t totalCount = countRow ? std::strtoll(countRow[0], nullptr, 10) : 0;

  // 2b. 再查当前页数据（LIMIT 分页；数值直接拼 SQL，无注入风险）
  std::string sql = "SELECT id, name, email, created_at FROM users ORDER BY id LIMIT " +
                    std::to_string(limit) + " OFFSET " + std::to_string(offset);
  MysqlResult res(conn->query(sql));
  if (!res) return false;

  // 逐行取结果，组装成 vector<User>
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

  // ---- 第 3 步：写缓存 + 登记索引 ----
  nlohmann::json j;
  j["total"] = totalCount;
  j["users"] = nlohmann::json::array();
  for (const auto& u : users) {
    j["users"].push_back(UserToJson(u));
  }
  redis_->set(cacheKey, j.dump(), kListCacheTtl);  // 列表本身缓存 10s
  redis_->sadd(kListCacheIndex, cacheKey);         // 把 key 记入索引，供写时失效

  *out = std::move(users);
  *total = totalCount;
  return true;
}

// 统计信息，两部分各用不同的 Redis 用法：
//   total_users  → COUNT(*) 的结果，可缓存（低写高频读），故走"缓存优先"；
//   request_count → 每次业务请求都自增，本质是计数器，直接读当前值即可。
bool UserRepository::GetStats(int64_t* total_users, int64_t* request_count) {
  // ---- total_users：缓存优先（TTL 30s）----
  auto cached = redis_->get("stats:total_users");
  if (cached) {
    *total_users = std::strtoll(cached->c_str(), nullptr, 10);  // 缓存命中
  } else {
    // 未命中：回源 COUNT(*) 并回填（与 GetUser 同一套路）
    auto guard = pool_->acquire();
    MysqlConn* conn = guard.get();
    MysqlResult res(conn->query("SELECT COUNT(*) FROM users"));
    if (!res) return false;
    MYSQL_ROW row = mysql_fetch_row(res.get());
    int64_t total = row ? std::strtoll(row[0], nullptr, 10) : 0;
    redis_->set("stats:total_users", std::to_string(total), kStatsCacheTtl);
    *total_users = total;
  }

  // ---- request_count：直接读 INCR 累计的当前值 ----
  auto rc = redis_->get("stats:request_count");
  *request_count = rc ? std::strtoll(rc->c_str(), nullptr, 10) : 0;  // 未计过则为 0
  return true;
}

}  // namespace http_server_demo
