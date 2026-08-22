#include "grpc_server/user_service.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/json_utils.h"

namespace http_server_demo {

namespace {

int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

UserServiceImpl::UserServiceImpl(std::shared_ptr<RedisClient> redis)
    : redis_(std::move(redis)) {}

bool UserServiceImpl::SaveUser(const user::v1::User& user) {
  std::string json = UserToJsonString(user);
  // 主数据：SET user:{id} = JSON
  if (!redis_->set("user:" + user.id(), json)) return false;
  // 索引集合：SADD users:ids {id}，供 ListUsers 遍历
  return redis_->sadd("users:ids", user.id());
}

bool UserServiceImpl::LoadUser(const std::string& id, user::v1::User* user) {
  auto value = redis_->get("user:" + id);
  if (!value) return false;
  try {
    JsonToUser(nlohmann::json::parse(*value), user);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void UserServiceImpl::CountRequest() {
  // 每收到一次业务 RPC 就 INCR，演示 Redis 原子计数
  redis_->incr("stats:request_count");
}

grpc::Status UserServiceImpl::CreateUser(grpc::ServerContext* context,
                                         const user::v1::CreateUserRequest* request,
                                         user::v1::CreateUserReply* reply) {
  CountRequest();

  if (request->name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "name is required");
  }

  // 用 Redis INCR 生成自增用户 ID，兼顾幂等与演示
  auto seq = redis_->incr("users:seq");
  if (!seq) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "redis INCR failed: " + redis_->lastError());
  }
  std::string id = "u" + std::to_string(*seq);

  user::v1::User user;
  user.set_id(id);
  user.set_name(request->name());
  user.set_email(request->email());
  user.set_created_at(NowUnixSeconds());

  if (!SaveUser(user)) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "redis write failed: " + redis_->lastError());
  }

  *reply->mutable_user() = std::move(user);
  return grpc::Status::OK;
}

grpc::Status UserServiceImpl::GetUser(grpc::ServerContext* context,
                                      const user::v1::GetUserRequest* request,
                                      user::v1::GetUserReply* reply) {
  CountRequest();

  if (request->id().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "id is required");
  }

  user::v1::User user;
  if (!LoadUser(request->id(), &user)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "user not found");
  }

  *reply->mutable_user() = std::move(user);
  return grpc::Status::OK;
}

grpc::Status UserServiceImpl::ListUsers(grpc::ServerContext* context,
                                        const user::v1::ListUsersRequest* request,
                                        user::v1::ListUsersReply* reply) {
  CountRequest();

  int limit = request->limit() > 0 ? request->limit() : 10;
  int offset = request->offset() > 0 ? request->offset() : 0;

  auto ids = redis_->smembers("users:ids");
  if (!ids) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "redis SMEMBERS failed: " + redis_->lastError());
  }

  std::vector<user::v1::User> users;
  users.reserve(ids->size());
  for (const auto& id : *ids) {
    user::v1::User user;
    if (LoadUser(id, &user)) {
      users.push_back(std::move(user));
    }
  }

  reply->set_total(static_cast<int64_t>(users.size()));
  for (int i = offset;
       i < static_cast<int>(users.size()) && i < offset + limit; ++i) {
    *reply->add_users() = users[i];
  }
  return grpc::Status::OK;
}

grpc::Status UserServiceImpl::GetStats(grpc::ServerContext* context,
                                       const user::v1::GetStatsRequest* request,
                                       user::v1::GetStatsReply* reply) {
  auto total = redis_->scard("users:ids");
  auto count = redis_->get("stats:request_count");

  reply->set_total_users(total.value_or(0));
  reply->set_request_count(count ? std::strtoll(count->c_str(), nullptr, 10) : 0);
  return grpc::Status::OK;
}

}  // namespace http_server_demo
