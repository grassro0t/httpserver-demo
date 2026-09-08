#pragma once

#include <mysql/mysql.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace http_server_demo {

// =============================================================================
// MySQL 连接配置
// =============================================================================
// 程序启动时按此配置建立连接池。
// pool_size：池中常驻的连接数量（默认 8）。每个 gRPC 处理线程需要读写
// MySQL 时，从池中借一个连接用完后归还，避免"每次请求都新建 TCP 连接 +
// 握手认证"的高昂开销。
struct MysqlConfig {
  std::string host = "127.0.0.1";             // MySQL 主机地址
  int port = 3306;                            // MySQL 端口
  std::string user = "root";                  // 用户名
  std::string password;                       // 密码（brew 默认 root 空密码）
  std::string database = "http_server_demo";  // 目标库名；为空则只连接不选库
  int pool_size = 8;                          // 连接池大小
};

// =============================================================================
// MYSQL_RES 的 RAII 封装（结果集的资源管理）
// =============================================================================
// 背景：MySQL C API 中，SELECT 的结果通过 mysql_store_result() 返回
// MYSQL_RES*，它是一块堆内存（包含所有行数据），用完必须 mysql_free_result()
// 手动释放，否则内存泄漏。
// 作用：把释放动作绑定到析构函数，对象离开作用域自动释放，且禁止拷贝，
// 从根上杜绝"忘记释放 / 重复释放"两类错误。
class MysqlResult {
 public:
  explicit MysqlResult(MYSQL_RES* res) : res_(res) {}
  ~MysqlResult() {
    if (res_) mysql_free_result(res_);
  }
  MysqlResult(const MysqlResult&) = delete;             // 拷贝=复制同一指针，会双重释放
  MysqlResult& operator=(const MysqlResult&) = delete;  // 禁止拷贝赋值

  MYSQL_RES* get() const { return res_; }
  // 允许 if (res) / if (!res) 这样判断结果集是否有效
  explicit operator bool() const { return res_ != nullptr; }

 private:
  MYSQL_RES* res_;
};

// =============================================================================
// 单条 MySQL 连接的 RAII 封装
// =============================================================================
// 包装 libmysqlclient 的 C 句柄 MYSQL*。持有唯一一条底层连接，职责：
//   1. 生命周期管理：构造不连接，connect() 显式连接，析构/close() 关闭；
//   2. 提供 execute/query/lastInsertId/escape 等高层、类型安全的方法；
//   3. 统一记录错误信息（err_），业务层不必直接面对 mysql_error()。
// 为什么禁止拷贝？MYSQL* 是一个资源句柄，若两个对象指向同一连接，其中一个
// 析构 close 后另一个就成了野指针。所以只能按"单所有权"使用。
class MysqlConn {
 public:
  MysqlConn() = default;  // 默认构造，不建立连接
  ~MysqlConn();           // 析构时自动 close（RAII 的关键）
  MysqlConn(const MysqlConn&) = delete;
  MysqlConn& operator=(const MysqlConn&) = delete;

  // 建立到 MySQL 的连接；失败返回 false 并记录 err_。可配置 database 为空
  // 以"不选库"方式连接（建库阶段需要这种连接）。
  bool connect(const MysqlConfig& cfg);
  // 关闭连接并置空句柄；重复调用安全（幂等）
  void close();
  bool isConnected() const { return mysql_ != nullptr; }

  // 执行不返回结果集的语句：INSERT / UPDATE / DELETE / CREATE 等
  // （不能用来执行 SELECT，SELECT 请用 query()）
  bool execute(const std::string& sql);
  // 执行 SELECT，返回结果集指针（调用方应立刻用 MysqlResult 包装以自动释放）
  // 与 execute() 的本质区别：SELECT 会产生结果集，必须 store_result 取出
  MYSQL_RES* query(const std::string& sql);
  // 返回上一次 INSERT 产生的自增主键（AUTO_INCREMENT），用于回填业务 ID
  unsigned long long lastInsertId();
  // 转义字符串中的特殊字符（引号、反斜杠等），用于把用户输入安全地拼进 SQL，
  // 防止 SQL 注入。转义后字符串不带引号，拼接时需自行加 'xxx'。
  std::string escape(const std::string& s);

  // 最近一次操作的错误信息（供上层打印/日志）
  const std::string& lastError() const { return err_; }

 private:
  MYSQL* mysql_ = nullptr;  // 底层 C 句柄；nullptr 表示当前无连接
  std::string err_;
};

// =============================================================================
// MySQL 连接池
// =============================================================================
// 【为什么要连接池】
//   a) MySQL 单条连接不是线程安全的：gRPC server 会多线程并发处理 RPC，
//      若多个线程共享同一个 MYSQL*，命令会交错导致数据错乱/崩溃。
//   b) 频繁建连开销大：每条连接都要 TCP 握手 + MySQL 认证，QPS 高时开销可观。
// 方案：启动时一次性建好 pool_size 条连接，全部放进空闲队列 idle_；
// 使用方通过 acquire() 借一条、用完由 Guard 析构自动归还。
// 多线程通过互斥锁 mu_ 保证"同一时刻一条连接只被一个线程使用"，从而线程安全。
//
// 【成员说明】
//   conns_  连接池真正持有（所有）连接对象，unique_ptr 保证生命周期与销毁；
//   idle_   当前空闲的连接指针栈（借用/归还的"货架"）；
//   mu_     保护 idle_ 与连接借还操作的互斥锁；
//   cv_     条件变量：当池被借空时，acquire() 阻塞等待，release() 归还时唤醒。
class MysqlPool {
 public:
  ~MysqlPool() { shutdown(); }  // 对象销毁时关闭所有连接

  // 按配置建立连接池并自动完成"建库 + 建表"；失败返回 false
  bool init(const MysqlConfig& cfg);
  // 关闭并释放全部连接（幂等；析构时也会调用）
  void shutdown();

  // =====================================================================
  // RAII 守卫：借用连接的"临时凭证"
  // =====================================================================
  // 典型用法：
  //   auto guard = pool->acquire();   // 借出一条连接
  //   MysqlConn* conn = guard.get();  // 拿到原始指针使用
  //   conn->execute("...");
  //   // guard 离开作用域 → 析构 → 自动把连接归还池子
  //
  // 为什么需要它？如果手动 acquire/release，中间发生异常或提前 return 就会
  // "借了不还"，连接越借越少最终池空。Guard 把"归还"绑到析构，保证异常安全。
  // 为什么禁止拷贝、允许移动？拷贝会让两个 Guard 指向同一条连接，析构两次
  // 归还 → 连接被重复放进 idle_。移动则把所有权移交（原对象置空不再归还）。
  class Guard {
   public:
    Guard(MysqlPool* pool, MysqlConn* conn) : pool_(pool), conn_(conn) {}
    // 移动构造：把 other 手里的连接转交给新对象；other.conn_ 置空，
    // 这样 other 析构时不会把同一连接再还一次。
    Guard(Guard&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
      other.conn_ = nullptr;
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() {
      if (conn_) pool_->release(conn_);  // 有连接才归还（被移动走则跳过）
    }

    MysqlConn* get() const { return conn_; }
    MysqlConn* operator->() const { return conn_; }  // 支持 guard->execute(...)

   private:
    MysqlPool* pool_;  // 归还到哪个池
    MysqlConn* conn_;  // 手里借的连接；nullptr 表示已无连接（被移动或已归还）
  };

  // 从池中借出一条空闲连接；若池空则阻塞等待直到有连接归还。
  // 返回 Guard（RAII），作用域结束自动归还。
  Guard acquire();
  const std::string& lastError() const { return err_; }

 private:
  void release(MysqlConn* conn);  // 归还连接（由 Guard 析构调用）

  MysqlConfig cfg_;                                // 保存配置，重建/日志使用
  std::vector<std::unique_ptr<MysqlConn>> conns_;  // 连接的所有权容器
  std::vector<MysqlConn*> idle_;                   // 空闲连接栈（借用/归还）
  std::mutex mu_;                                  // 保护 idle_ 的互斥锁
  std::condition_variable cv_;                     // 池空时阻塞/归还时唤醒
  std::string err_;
};

}  // namespace http_server_demo
