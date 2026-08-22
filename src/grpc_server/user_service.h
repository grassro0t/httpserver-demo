#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "store/redis_client.h"
#include "user/v1/user.grpc.pb.h"

namespace http_server_demo {

// UserService 的 gRPC 实现：业务逻辑 + Redis 读写
class UserServiceImpl final : public user::v1::UserService::Service {
 public:
  explicit UserServiceImpl(std::shared_ptr<RedisClient> redis);

  grpc::Status CreateUser(grpc::ServerContext* context,
                          const user::v1::CreateUserRequest* request,
                          user::v1::CreateUserReply* reply) override;

  grpc::Status GetUser(grpc::ServerContext* context,
                       const user::v1::GetUserRequest* request,
                       user::v1::GetUserReply* reply) override;

  grpc::Status ListUsers(grpc::ServerContext* context,
                         const user::v1::ListUsersRequest* request,
                         user::v1::ListUsersReply* reply) override;

  grpc::Status GetStats(grpc::ServerContext* context,
                        const user::v1::GetStatsRequest* request,
                        user::v1::GetStatsReply* reply) override;

 private:
  bool SaveUser(const user::v1::User& user);
  bool LoadUser(const std::string& id, user::v1::User* user);
  void CountRequest();

  std::shared_ptr<RedisClient> redis_;
};

}  // namespace http_server_demo
