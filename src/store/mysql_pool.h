#pragma once

#include <mysql/mysql.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace http_server_demo {

struct MysqlConfig {
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user = "root";
  std::string password;
  std::string database = "http_server_demo";
  int pool_size = 8;
};

// MYSQL_RES 的 RAII 封装：析构自动 mysql_free_result
class MysqlResult {
 public:
  explicit MysqlResult(MYSQL_RES* res) : res_(res) {}
  ~MysqlResult() {
    if (res_) mysql_free_result(res_);
  }
  MysqlResult(const MysqlResult&) = delete;
  MysqlResult& operator=(const MysqlResult&) = delete;

  MYSQL_RES* get() const { return res_; }
  explicit operator bool() const { return res_ != nullptr; }

 private:
  MYSQL_RES* res_;
};

// 单条 MySQL 连接的 RAII 封装（基于 libmysqlclient C API）
class MysqlConn {
 public:
  MysqlConn() = default;
  ~MysqlConn();
  MysqlConn(const MysqlConn&) = delete;
  MysqlConn& operator=(const MysqlConn&) = delete;

  bool connect(const MysqlConfig& cfg);
  void close();
  bool isConnected() const { return mysql_ != nullptr; }

  // 执行非查询语句（INSERT/UPDATE/DELETE/CREATE 等）
  bool execute(const std::string& sql);
  // 执行查询语句，返回结果集（用 MysqlResult 包装）
  MYSQL_RES* query(const std::string& sql);
  // 上一条 INSERT 的自增主键
  unsigned long long lastInsertId();
  // 转义字符串，防 SQL 注入
  std::string escape(const std::string& s);

  const std::string& lastError() const { return err_; }

 private:
  MYSQL* mysql_ = nullptr;
  std::string err_;
};

// MySQL 连接池：固定大小 + 空闲队列，Acquire 借用 / Guard 析构归还
class MysqlPool {
 public:
  ~MysqlPool() { shutdown(); }

  bool init(const MysqlConfig& cfg);
  void shutdown();

  // RAII 守卫：借用连接，析构自动归还
  class Guard {
   public:
    Guard(MysqlPool* pool, MysqlConn* conn) : pool_(pool), conn_(conn) {}
    Guard(Guard&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
      other.conn_ = nullptr;
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() {
      if (conn_) pool_->release(conn_);
    }

    MysqlConn* get() const { return conn_; }
    MysqlConn* operator->() const { return conn_; }

   private:
    MysqlPool* pool_;
    MysqlConn* conn_;
  };

  Guard acquire();
  const std::string& lastError() const { return err_; }

 private:
  void release(MysqlConn* conn);

  MysqlConfig cfg_;
  std::vector<std::unique_ptr<MysqlConn>> conns_;
  std::vector<MysqlConn*> idle_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::string err_;
};

}  // namespace http_server_demo
