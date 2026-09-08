// redis_client.cpp —— Redis 缓存客户端的实现
//
// hiredis 是 C 库，所有命令函数返回 void*、错误通过 context->err/errstr 报告、
// 结果通过堆分配的 redisReply* 返回。本文件把这些"非 C++ 风格"的细节
// 全部收敛到各方法内部，业务层拿到的都是 std::optional / bool / 字符串。
#include "store/redis_client.h"

#include <hiredis/hiredis.h>

#include <cstdarg>
#include <cstring>

namespace http_server_demo {

// ---------------------------------------------------------------------------
// Command：所有命令的"统一出口"
// ---------------------------------------------------------------------------
// 为什么需要它？hiredis 为了 C 兼容，把 redisCommand/redisvCommand 的返回
// 类型声明成 void*（而不是 redisReply*），每次调用都要 static_cast 很啰嗦；
// 且变参函数（fmt, ...）无法直接转发给另一个变参函数，需要 va_list 中转。
// 这里统一：
//   1. 接收 printf 风格格式串（如 "SET %s %s EX %d"）与变参；
//   2. 通过 va_list 调用 redisvCommand（v 前缀= 接收 va_list 版本）；
//   3. 把 void* 强转回 redisReply* 返回。
// 注意：%s 对应的实参必须传 char*（std::string 需先 .c_str()），
// 且 key/value 里若含 % 不会被二次解析（它们作为 %s 的值传入）。
redisReply* RedisClient::Command(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);  // 开始收集变参
  redisReply* reply = static_cast<redisReply*>(redisvCommand(ctx_, fmt, args));
  va_end(args);  // 结束（配对必须）
  return reply;
}

RedisClient::~RedisClient() { close(); }  // 对象销毁时自动断开（RAII）

// 建立连接。使用带超时的版本 redisConnectWithTimeout，
// 避免 Redis 不在线时客户端无限期卡在 connect 上。
bool RedisClient::connect(const std::string& host, int port, int timeout_sec) {
  if (ctx_) close();  // 已连接则先断开，保证可安全重复调用

  struct timeval timeout{};  // hiredis 用 timeval 表示超时
  timeout.tv_sec = timeout_sec;

  ctx_ = redisConnectWithTimeout(host.c_str(), port, timeout);
  // 两种失败可能：
  //   a) 返回 nullptr —— 连内存分配都失败；
  //   b) ctx_->err != 0 —— TCP 连接/握手失败，errstr 里有原因。
  if (!ctx_ || ctx_->err) {
    err_ = ctx_ ? std::string(ctx_->errstr) : "redisConnectWithTimeout returned null";
    close();  // 半成品 ctx_ 也要释放，避免泄漏
    return false;
  }
  err_.clear();
  return true;
}

// 断开连接并置空句柄；置空后 isConnected() 为 false，重复调用安全。
void RedisClient::close() {
  if (ctx_) {
    redisFree(ctx_);  // 释放连接及内部读写缓冲区
    ctx_ = nullptr;
  }
}

// SET key value [EX ttl]：写字符串值。
// ttl_sec > 0 时附带 EX —— Redis 会在 ttl 秒后自动删除该键，
// 这是缓存"自动失效"的基础（本 demo 单用户缓存 TTL=60s）。
// 返回的 reply 是 "OK"（REDIS_REPLY_STATUS），用 r.ok() 判断即可。
bool RedisClient::set(const std::string& key, const std::string& value, int ttl_sec) {
  RedisReply r(ttl_sec > 0 ? Command("SET %s %s EX %d", key.c_str(), value.c_str(), ttl_sec)
                           : Command("SET %s %s", key.c_str(), value.c_str()));
  if (!r.ok()) {
    err_ = r.error().empty() ? "SET failed" : r.error();
    return false;
  }
  return true;
}

// GET key：读字符串值。
// 注意区分两种"取不到"：
//   a) 键不存在 → reply 类型 REDIS_REPLY_NIL → 返回 std::nullopt；
//   b) 键存在但类型不是字符串（如存的是列表）→ 类型不符 → 也返回 nullopt。
// 返回 std::optional 的意义就在于此：能区分"没有值"和"空字符串"。
std::optional<std::string> RedisClient::get(const std::string& key) {
  RedisReply r(Command("GET %s", key.c_str()));
  if (!r.ok()) {
    err_ = r.error().empty() ? "GET failed" : r.error();
    return std::nullopt;
  }
  if (r.get()->type == REDIS_REPLY_NIL) return std::nullopt;  // 键不存在
  if (r.get()->type != REDIS_REPLY_STRING) return std::nullopt;
  return std::string(r.get()->str, r.get()->len);  // 按 str+len 构造，兼容二进制安全
}

// DEL key：删除键。用于"写时失效缓存"（CreateUser 后删旧列表/统计缓存）。
bool RedisClient::del(const std::string& key) {
  RedisReply r(Command("DEL %s", key.c_str()));
  return r.ok();  // 删没删到都算命令执行成功
}

// EXISTS key：键是否存在。reply 是整数 1/0。
bool RedisClient::exists(const std::string& key) {
  RedisReply r(Command("EXISTS %s", key.c_str()));
  return r.ok() && r.get()->type == REDIS_REPLY_INTEGER && r.get()->integer == 1;
}

// INCR key：把键值加 1 并返回加后的值（原子操作，Redis 保证并发安全）。
// 本 demo 用它做两件事：生成自增 id（users:seq）、请求计数（stats:request_count）。
// 返回值是 REDIS_REPLY_INTEGER（存于 integer 字段）。
std::optional<int64_t> RedisClient::incr(const std::string& key) {
  RedisReply r(Command("INCR %s", key.c_str()));
  if (!r.ok() || r.get()->type != REDIS_REPLY_INTEGER) {
    err_ = r.error().empty() ? "INCR failed" : r.error();
    return std::nullopt;
  }
  return r.get()->integer;
}

// SADD key member：向集合加入一个成员。用于维护"列表缓存 key 索引"集合，
// 这样新增用户时能枚举出所有列表缓存 key 再逐个 DEL（写时失效）。
bool RedisClient::sadd(const std::string& key, const std::string& member) {
  RedisReply r(Command("SADD %s %s", key.c_str(), member.c_str()));
  return r.ok();
}

// SREM key member：从集合移除一个成员。
bool RedisClient::srem(const std::string& key, const std::string& member) {
  RedisReply r(Command("SREM %s %s", key.c_str(), member.c_str()));
  return r.ok();
}

// SMEMBERS key：取集合全部成员。
// reply 是 REDIS_REPLY_ARRAY：elements 为个数，element[i] 是子 reply，
// 逐个取出元素里的 str/len 组装成 std::vector<std::string>。
std::optional<std::vector<std::string>> RedisClient::smembers(const std::string& key) {
  RedisReply r(Command("SMEMBERS %s", key.c_str()));
  if (!r.ok() || r.get()->type != REDIS_REPLY_ARRAY) return std::nullopt;
  std::vector<std::string> result;
  result.reserve(r.get()->elements);  // 预分配，避免多次扩容
  for (size_t i = 0; i < r.get()->elements; ++i) {
    redisReply* ele = r.get()->element[i];
    if (ele->type == REDIS_REPLY_STRING) {
      result.emplace_back(ele->str, ele->len);
    }
  }
  return result;
}

// SCARD key：返回集合大小（成员个数）。reply 为整数。
std::optional<int64_t> RedisClient::scard(const std::string& key) {
  RedisReply r(Command("SCARD %s", key.c_str()));
  if (!r.ok() || r.get()->type != REDIS_REPLY_INTEGER) return std::nullopt;
  return r.get()->integer;
}

}  // namespace http_server_demo
