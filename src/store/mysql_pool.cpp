#include "store/mysql_pool.h"

namespace http_server_demo {

namespace {

constexpr const char* kCreateTableSql =
    "CREATE TABLE IF NOT EXISTS users ("
    "  id BIGINT AUTO_INCREMENT PRIMARY KEY,"
    "  name VARCHAR(128) NOT NULL,"
    "  email VARCHAR(256) NOT NULL,"
    "  created_at BIGINT NOT NULL"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

}  // namespace

MysqlConn::~MysqlConn() { close(); }

bool MysqlConn::connect(const MysqlConfig& cfg) {
  if (mysql_) close();

  mysql_ = mysql_init(nullptr);
  if (!mysql_) {
    err_ = "mysql_init failed";
    return false;
  }

  const char* db = cfg.database.empty() ? nullptr : cfg.database.c_str();
  if (!mysql_real_connect(mysql_, cfg.host.c_str(), cfg.user.c_str(),
                          cfg.password.c_str(), db, cfg.port, nullptr, 0)) {
    err_ = mysql_error(mysql_);
    close();
    return false;
  }

  mysql_set_character_set(mysql_, "utf8mb4");
  err_.clear();
  return true;
}

void MysqlConn::close() {
  if (mysql_) {
    mysql_close(mysql_);
    mysql_ = nullptr;
  }
}

bool MysqlConn::execute(const std::string& sql) {
  if (mysql_query(mysql_, sql.c_str()) != 0) {
    err_ = mysql_error(mysql_);
    return false;
  }
  return true;
}

MYSQL_RES* MysqlConn::query(const std::string& sql) {
  if (mysql_query(mysql_, sql.c_str()) != 0) {
    err_ = mysql_error(mysql_);
    return nullptr;
  }
  MYSQL_RES* res = mysql_store_result(mysql_);
  if (!res && mysql_field_count(mysql_) > 0) {
    err_ = mysql_error(mysql_);
  }
  return res;
}

unsigned long long MysqlConn::lastInsertId() { return mysql_insert_id(mysql_); }

std::string MysqlConn::escape(const std::string& s) {
  std::vector<char> buf(s.size() * 2 + 1);
  mysql_real_escape_string(mysql_, buf.data(), s.c_str(), s.size());
  return std::string(buf.data());
}

bool MysqlPool::init(const MysqlConfig& cfg) {
  cfg_ = cfg;

  // 1. 用临时连接创建数据库（不带库名连接）
  {
    MysqlConn tmp;
    MysqlConfig no_db = cfg_;
    no_db.database.clear();
    if (!tmp.connect(no_db)) {
      err_ = "建库连接失败: " + tmp.lastError();
      return false;
    }
    std::string sql = "CREATE DATABASE IF NOT EXISTS `" + cfg_.database +
                      "` DEFAULT CHARACTER SET utf8mb4";
    if (!tmp.execute(sql)) {
      err_ = "CREATE DATABASE 失败: " + tmp.lastError();
      return false;
    }
  }

  // 2. 建立固定大小的连接池，每个连接带数据库名
  for (int i = 0; i < cfg_.pool_size; ++i) {
    auto conn = std::make_unique<MysqlConn>();
    if (!conn->connect(cfg_)) {
      err_ = "连接池初始化失败: " + conn->lastError();
      return false;
    }
    if (i == 0) {
      if (!conn->execute(kCreateTableSql)) {
        err_ = "CREATE TABLE 失败: " + conn->lastError();
        return false;
      }
    }
    idle_.push_back(conn.get());
    conns_.push_back(std::move(conn));
  }
  return true;
}

void MysqlPool::shutdown() {
  std::lock_guard<std::mutex> lk(mu_);
  idle_.clear();
  conns_.clear();
}

MysqlPool::Guard MysqlPool::acquire() {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [this] { return !idle_.empty(); });
  MysqlConn* conn = idle_.back();
  idle_.pop_back();
  return Guard(this, conn);
}

void MysqlPool::release(MysqlConn* conn) {
  std::lock_guard<std::mutex> lk(mu_);
  idle_.push_back(conn);
  cv_.notify_one();
}

}  // namespace http_server_demo
