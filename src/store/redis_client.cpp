#include "store/redis_client.h"

#include <hiredis/hiredis.h>

#include <cstdarg>
#include <cstring>

namespace http_server_demo {

redisReply* RedisClient::Command(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  redisReply* reply = static_cast<redisReply*>(redisvCommand(ctx_, fmt, args));
  va_end(args);
  return reply;
}

RedisClient::~RedisClient() { close(); }

bool RedisClient::connect(const std::string& host, int port, int timeout_sec) {
  if (ctx_) close();

  struct timeval timeout{};
  timeout.tv_sec = timeout_sec;

  ctx_ = redisConnectWithTimeout(host.c_str(), port, timeout);
  if (!ctx_ || ctx_->err) {
    err_ = ctx_ ? std::string(ctx_->errstr) : "redisConnectWithTimeout returned null";
    close();
    return false;
  }
  err_.clear();
  return true;
}

void RedisClient::close() {
  if (ctx_) {
    redisFree(ctx_);
    ctx_ = nullptr;
  }
}

bool RedisClient::set(const std::string& key, const std::string& value, int ttl_sec) {
  RedisReply r(ttl_sec > 0 ? Command("SET %s %s EX %d", key.c_str(), value.c_str(), ttl_sec)
                           : Command("SET %s %s", key.c_str(), value.c_str()));
  if (!r.ok()) {
    err_ = r.error().empty() ? "SET failed" : r.error();
    return false;
  }
  return true;
}

std::optional<std::string> RedisClient::get(const std::string& key) {
  RedisReply r(Command("GET %s", key.c_str()));
  if (!r.ok()) {
    err_ = r.error().empty() ? "GET failed" : r.error();
    return std::nullopt;
  }
  if (r.get()->type == REDIS_REPLY_NIL) return std::nullopt;  // 键不存在
  if (r.get()->type != REDIS_REPLY_STRING) return std::nullopt;
  return std::string(r.get()->str, r.get()->len);
}

bool RedisClient::del(const std::string& key) {
  RedisReply r(Command("DEL %s", key.c_str()));
  return r.ok();
}

bool RedisClient::exists(const std::string& key) {
  RedisReply r(Command("EXISTS %s", key.c_str()));
  return r.ok() && r.get()->type == REDIS_REPLY_INTEGER && r.get()->integer == 1;
}

std::optional<int64_t> RedisClient::incr(const std::string& key) {
  RedisReply r(Command("INCR %s", key.c_str()));
  if (!r.ok() || r.get()->type != REDIS_REPLY_INTEGER) {
    err_ = r.error().empty() ? "INCR failed" : r.error();
    return std::nullopt;
  }
  return r.get()->integer;
}

bool RedisClient::sadd(const std::string& key, const std::string& member) {
  RedisReply r(Command("SADD %s %s", key.c_str(), member.c_str()));
  return r.ok();
}

bool RedisClient::srem(const std::string& key, const std::string& member) {
  RedisReply r(Command("SREM %s %s", key.c_str(), member.c_str()));
  return r.ok();
}

std::optional<std::vector<std::string>> RedisClient::smembers(const std::string& key) {
  RedisReply r(Command("SMEMBERS %s", key.c_str()));
  if (!r.ok() || r.get()->type != REDIS_REPLY_ARRAY) return std::nullopt;
  std::vector<std::string> result;
  result.reserve(r.get()->elements);
  for (size_t i = 0; i < r.get()->elements; ++i) {
    redisReply* ele = r.get()->element[i];
    if (ele->type == REDIS_REPLY_STRING) {
      result.emplace_back(ele->str, ele->len);
    }
  }
  return result;
}

std::optional<int64_t> RedisClient::scard(const std::string& key) {
  RedisReply r(Command("SCARD %s", key.c_str()));
  if (!r.ok() || r.get()->type != REDIS_REPLY_INTEGER) return std::nullopt;
  return r.get()->integer;
}

}  // namespace http_server_demo
