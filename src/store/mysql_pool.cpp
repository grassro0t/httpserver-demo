// mysql_pool.cpp —— MySQL 连接池的实现
//
// 整体职责分两块：
//   1) MysqlConn：单条连接的生命周期与底层 C API 封装（本文件前半部分）；
//   2) MysqlPool：多线程环境下"建池 / 借 / 还"的并发控制（本文件后半部分）。
// 设计顺序建议阅读：init()（建池）→ acquire()/release()（借还）→
// connect()/execute()/query()（单连接细节）。
#include "store/mysql_pool.h"

namespace http_server_demo {

namespace {

// 业务表结构。只定义一次，由 init() 在建池时对第一个连接执行
// "CREATE TABLE IF NOT EXISTS"，因此程序可重复启动而不会报"表已存在"。
constexpr const char* kCreateTableSql =
    "CREATE TABLE IF NOT EXISTS users ("
    "  id BIGINT AUTO_INCREMENT PRIMARY KEY,"  // 自增主键，由 lastInsertId() 取回
    "  name VARCHAR(128) NOT NULL,"
    "  email VARCHAR(256) NOT NULL,"
    "  created_at BIGINT NOT NULL"              // 秒级时间戳（Unix）
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";  // InnoDB 事务引擎 + utf8mb4 全字符集

}  // namespace

MysqlConn::~MysqlConn() { close(); }  // 对象销毁时确保底层连接被关闭（RAII）

// 建立连接的三步：初始化句柄 → 真实连接 → 设置字符集
bool MysqlConn::connect(const MysqlConfig& cfg) {
  if (mysql_) close();  // 若已持有连接则先关掉，保证幂等（可安全重复调用）

  // 1) mysql_init 分配并初始化一个 MYSQL 句柄（返回的仍是 mysql_）
  mysql_ = mysql_init(nullptr);
  if (!mysql_) {
    err_ = "mysql_init failed";  // 极少失败（如内存不足）
    return false;
  }

  // 2) mysql_real_connect 真正建立 TCP 连接并完成认证。
  //    参数依次：句柄 / 主机 / 用户 / 密码 / 数据库名 / 端口 / unix套接字(用TCP传nullptr) /
  //    客户端标志 关键点：cfg.database 为空时传 nullptr —— 表示"先不选库"，
  //    建库阶段（此时库还不存在）必须这样连，选库了反而连不上。
  const char* db = cfg.database.empty() ? nullptr : cfg.database.c_str();
  if (!mysql_real_connect(mysql_, cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), db,
                          cfg.port, nullptr, 0)) {
    err_ = mysql_error(mysql_);  // 认证失败/网络不通等，取错误说明
    close();                     // 失败要清理半成品句柄，避免泄漏
    return false;
  }

  // 3) 强制字符集 utf8mb4：若不做这一步，客户端默认 latin1 会把中文存成乱码
  mysql_set_character_set(mysql_, "utf8mb4");
  err_.clear();
  return true;
}

// 关闭连接并把句柄置空。置空后 isConnected() 返回 false，
// 重复调用也安全（close 过再 close 什么都不做）。
void MysqlConn::close() {
  if (mysql_) {
    mysql_close(mysql_);  // 关闭 TCP 连接并释放内部资源
    mysql_ = nullptr;
  }
}

// 执行"无结果集"语句（INSERT/UPDATE/DELETE/CREATE...）。
// mysql_query 发送 SQL 并等待服务端执行完成；返回 0 表示成功。
bool MysqlConn::execute(const std::string& sql) {
  if (mysql_query(mysql_, sql.c_str()) != 0) {
    err_ = mysql_error(mysql_);  // 语法错误/约束冲突等都走到这里
    return false;
  }
  return true;
}

// 执行 SELECT 并取回结果集。
// 与 execute() 的区别：SELECT 会返回多行数据（结果集）。
// 注意：SELECT 后若不调用 mysql_store_result 把结果取走，这条连接的协议
// 状态会停留在"还有结果未读"，下次再发任何命令都会报
// "Commands out of sync"——所以这里必须 store_result。
MYSQL_RES* MysqlConn::query(const std::string& sql) {
  if (mysql_query(mysql_, sql.c_str()) != 0) {
    err_ = mysql_error(mysql_);
    return nullptr;
  }
  MYSQL_RES* res = mysql_store_result(mysql_);  // 把结果整块拉回内存
  // mysql_store_result 对"SELECT 但没匹配行"也会返回有效(空)结果集；
  // 仅当确实发生错误且当前语句带字段(说明是查询类)时才记错误。
  if (!res && mysql_field_count(mysql_) > 0) {
    err_ = mysql_error(mysql_);
  }
  return res;  // 调用方应立即用 MysqlResult 包装，保证自动释放
}

// 返回上一条 INSERT 语句生成的自增主键（AUTO_INCREMENT 列）。
// 用于把数据库分配的 id 回填给业务对象。
unsigned long long MysqlConn::lastInsertId() { return mysql_insert_id(mysql_); }

// 防 SQL 注入的核心工具。
// mysql_real_escape_string 会转义单引号、双引号、反斜杠、换行等特殊字符。
// 缓冲区按"2 倍长度 + 1"分配：最坏情况下每个字节都可能变成 2 个转义字节。
// 转义后的字符串不含外层引号，拼接 SQL 时需自行包上 'xxx'。
std::string MysqlConn::escape(const std::string& s) {
  std::vector<char> buf(s.size() * 2 + 1);
  mysql_real_escape_string(mysql_, buf.data(), s.c_str(), s.size());
  return std::string(buf.data());
}

// 初始化连接池：分两大步 —— 先建库，再批量建连接。
// 任何一步失败都直接返回 false（main 里会打印 err_ 后退出）。
bool MysqlPool::init(const MysqlConfig& cfg) {
  cfg_ = cfg;

  // ---------- 第 1 步：确保数据库存在 ----------
  // 用一个临时连接，且故意"不选库"（no_db.database 清空）——
  // 因为此时库可能还不存在，选了库反而连接失败。
  {
    MysqlConn tmp;
    MysqlConfig no_db = cfg_;
    no_db.database.clear();
    if (!tmp.connect(no_db)) {
      err_ = "建库连接失败: " + tmp.lastError();
      return false;
    }
    // CREATE DATABASE IF NOT EXISTS：库已存在也不报错，可重复启动
    std::string sql =
        "CREATE DATABASE IF NOT EXISTS `" + cfg_.database + "` DEFAULT CHARACTER SET utf8mb4";
    if (!tmp.execute(sql)) {
      err_ = "CREATE DATABASE 失败: " + tmp.lastError();
      return false;
    }
  }  // tmp 在此析构，自动 close —— 建库连接用完即弃

  // ---------- 第 2 步：批量建立池内连接 ----------
  for (int i = 0; i < cfg_.pool_size; ++i) {
    auto conn = std::make_unique<MysqlConn>();  // 每条连接独立 new，独立管理
    if (!conn->connect(cfg_)) {                 // 此时带库名连接
      err_ = "连接池初始化失败: " + conn->lastError();
      return false;  // 失败时 conns_ 中的连接由 unique_ptr 自动析构关闭
    }
    // 只对第 1 条连接执行一次建表（IF NOT EXISTS，幂等），
    // 其余连接无需重复建表 —— 表是数据库全局的，建一次即可。
    if (i == 0) {
      if (!conn->execute(kCreateTableSql)) {
        err_ = "CREATE TABLE 失败: " + conn->lastError();
        return false;
      }
    }
    idle_.push_back(conn.get());        // 空闲指针入栈：可被 acquire() 借出
    conns_.push_back(std::move(conn));  // 真正持有对象，防止中途析构
  }
  return true;
}

// 关闭连接池：清空空闲栈与持有容器，所有 MysqlConn 析构 → close() → 释放。
// 加锁是因为理论上可能与 acquire/release 并发；MysqlPool 析构时也会调用。
void MysqlPool::shutdown() {
  std::lock_guard<std::mutex> lk(mu_);
  idle_.clear();
  conns_.clear();
}

// 从池中借一条空闲连接（把连接"拿走"）。
// 用 unique_lock 而非 lock_guard 的原因：cv_.wait 需要在等待期间自动解锁，
// 等条件满足后再重新加锁 —— 这是条件变量等待的标准用法。
MysqlPool::Guard MysqlPool::acquire() {
  std::unique_lock<std::mutex> lk(mu_);
  // 若池空（所有连接都被借走），当前线程阻塞等待；
  // 其他线程 release() 归还时会 notify_one() 唤醒这里。
  // 谓词 !idle_.empty() 用于处理"假唤醒"：被唤醒后重新检查条件，不满足继续睡。
  cv_.wait(lk, [this] { return !idle_.empty(); });
  MysqlConn* conn = idle_.back();  // 取栈顶一条
  idle_.pop_back();                // 出栈 = 借出（idle_ 不再持有它）
  return Guard(this, conn);        // 用 Guard 包装返回，析构时自动还回
}

// 归还连接（由 Guard 析构调用，业务代码不应直接调用）。
void MysqlPool::release(MysqlConn* conn) {
  std::lock_guard<std::mutex> lk(mu_);
  idle_.push_back(conn);  // 放回空闲栈
  cv_.notify_one();       // 唤醒一个正在 acquire() 中等待的线程
}

}  // namespace http_server_demo
