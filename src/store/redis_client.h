#pragma once

#include <hiredis/hiredis.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace http_server_demo {

// 基于 hiredis(C API) 的轻量 RAII 封装。
// RedisReply 负责自动释放 redisReply*；RedisClient 封装常用命令。
// 设计意图：屏蔽 hiredis 的 C 接口细节，业务层只面对现代 C++ API。

class RedisReply {
 public:
  explicit RedisReply(redisReply* reply) : reply_(reply) {}
  ~RedisReply() {
    if (reply_) freeReplyObject(reply_);
  }
  RedisReply(const RedisReply&) = delete;
  RedisReply& operator=(const RedisReply&) = delete;

  redisReply* get() const { return reply_; }

  // 是否成功（非 ERROR 类型）
  bool ok() const { return reply_ != nullptr && reply_->type != REDIS_REPLY_ERROR; }

  // 错误信息
  std::string error() const {
    return (reply_ != nullptr && reply_->type == REDIS_REPLY_ERROR)
               ? std::string(reply_->str, reply_->len)
               : std::string();
  }

 private:
  redisReply* reply_;
};

class RedisClient {
 public:
  RedisClient() = default;
  ~RedisClient();

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  // 连接 Redis，timeout_sec 为连接超时
  bool connect(const std::string& host, int port, int timeout_sec = 2);

  void close();
  bool isConnected() const { return ctx_ != nullptr; }

  const std::string& lastError() const { return err_; }

  // ---- 基础命令 ----
  bool set(const std::string& key, const std::string& value, int ttl_sec = 0);
  std::optional<std::string> get(const std::string& key);
  bool del(const std::string& key);
  bool exists(const std::string& key);

  // INCR，返回自增后的值
  std::optional<int64_t> incr(const std::string& key);

  // SET 集合操作（用于维护用户 ID 集合）
  bool sadd(const std::string& key, const std::string& member);
  bool srem(const std::string& key, const std::string& member);
  std::optional<std::vector<std::string>> smembers(const std::string& key);
  std::optional<int64_t> scard(const std::string& key);

 private:
  // hiredis 的 redisCommand 返回 void*，这里统一包一层并转换为 redisReply*
  redisReply* Command(const char* fmt, ...);
  redisContext* ctx_ = nullptr;
  std::string err_;
};

}  // namespace http_server_demo
