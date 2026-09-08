#pragma once

#include <hiredis/hiredis.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace http_server_demo {

// =============================================================================
// Redis 客户端（本项目中 Redis 是"缓存层"）
// =============================================================================
// 与 mysql_pool 的定位不同：MySQL 存全量数据（持久层），Redis 只做
// Cache-Aside 缓存（可随时清空重建），因此本文件是对 hiredis(C API) 的
// 一层轻量封装，只实现缓存用到的几个命令。
//
// 使用方（UserRepository）典型流程：
//   GetUser:  GET user:{id} → 命中直接返回；未命中才去查 MySQL 并 SET 回填
//   CreateUser: SET user:{id}(带TTL) + 失效列表缓存 + INCR 统计
//
// 注意（线程安全）：hiredis 的 redisContext 不是线程安全的。本 demo 中
// grpc-server 多个 RPC 线程会共享一个 RedisClient → 单连接下有数据竞争风险，
// 属于教学上的简化；若要严格并发，应仿照 MysqlPool 做连接池或 thread_local
// 每线程一条连接（这也是本项目 mysql 侧已经解决的问题）。
// =============================================================================

// =============================================================================
// redisReply 的 RAII 封装
// =============================================================================
// hiredis 每个命令都会返回一个堆分配的 redisReply*（reply 结构体），
// 用完必须 freeReplyObject() 释放。RedisReply 把释放绑到析构：
//   1. 杜绝"忘记释放"导致的内存泄漏；
//   2. 禁止拷贝 —— 拷贝会让两个对象持有同一 reply，析构双重释放。
// redisReply 是一个带 type 标签的联合体式结构，字段含义随 type 变化：
//   REDIS_REPLY_STRING  → str/len（字符串）
//   REDIS_REPLY_INTEGER → integer（整数，INCR/SCARD 等）
//   REDIS_REPLY_ARRAY   → elements/element[]（数组，SMEMBERS 等）
//   REDIS_REPLY_NIL     → 空结果（如 GET 不存在的键）
//   REDIS_REPLY_ERROR   → 服务端返回错误（str 里是错误消息）
class RedisReply {
 public:
  explicit RedisReply(redisReply* reply) : reply_(reply) {}
  ~RedisReply() {
    if (reply_) freeReplyObject(reply_);  // RAII：离开作用域自动释放
  }
  RedisReply(const RedisReply&) = delete;
  RedisReply& operator=(const RedisReply&) = delete;

  redisReply* get() const { return reply_; }

  // 是否成功：reply 有效 且 不是 ERROR 类型
  bool ok() const { return reply_ != nullptr && reply_->type != REDIS_REPLY_ERROR; }

  // 提取错误消息（仅当 reply 是 ERROR 类型时有意义）
  std::string error() const {
    return (reply_ != nullptr && reply_->type == REDIS_REPLY_ERROR)
               ? std::string(reply_->str, reply_->len)
               : std::string();
  }

 private:
  redisReply* reply_;  // 底层 C reply 指针；由析构负责释放
};

// =============================================================================
// RedisClient：缓存命令封装
// =============================================================================
// 持有唯一一条 hiredis 连接（redisContext*），把"字符串命令"翻译成
// 类型安全、返回 std::optional 的 C++ 方法：
//   - 键不存在 / 类型不符 → 返回 std::nullopt / false（语义化，不抛异常）
//   - 失败原因统一记在 err_，用 lastError() 读取
// 与 MysqlConn 一样：持有资源句柄，禁止拷贝（单所有权）。
class RedisClient {
 public:
  RedisClient() = default;  // 默认构造，不建立连接
  ~RedisClient();           // 析构自动断开（RAII）

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  // 建立 TCP 连接；timeout_sec 为连接超时（秒）。失败返回 false 并记 err_
  bool connect(const std::string& host, int port, int timeout_sec = 2);

  void close();  // 断开并置空句柄（幂等）
  bool isConnected() const { return ctx_ != nullptr; }

  const std::string& lastError() const { return err_; }

  // ---- 基础缓存命令 ----
  // 写字符串值；ttl_sec > 0 时附加 EX 过期时间（缓存自动失效的核心）
  bool set(const std::string& key, const std::string& value, int ttl_sec = 0);
  // 读字符串值；键不存在返回 std::nullopt（区别于"空串"）
  std::optional<std::string> get(const std::string& key);
  // 删除键（写时失效缓存用）；返回是否执行成功
  bool del(const std::string& key);
  // 判断键是否存在
  bool exists(const std::string& key);

  // 原子自增并返回自增后的值（用于请求计数 stats:request_count）
  std::optional<int64_t> incr(const std::string& key);

  // ---- Set 集合命令（维护"列表缓存 key 索引"，用于写时批量失效）----
  bool sadd(const std::string& key, const std::string& member);              // 加入成员
  bool srem(const std::string& key, const std::string& member);              // 移除成员
  std::optional<std::vector<std::string>> smembers(const std::string& key);  // 取全部成员
  std::optional<int64_t> scard(const std::string& key);                      // 集合大小

 private:
  // hiredis 的 redisCommand/redisvCommand 返回 void*（C 兼容设计），
  // 这里统一包成 redisReply* 返回，免去每个调用处强转；变参转发给
  // redisvCommand（详见 .cpp）。
  redisReply* Command(const char* fmt, ...);
  redisContext* ctx_ = nullptr;  // 底层连接句柄；nullptr = 未连接
  std::string err_;
};

}  // namespace http_server_demo
