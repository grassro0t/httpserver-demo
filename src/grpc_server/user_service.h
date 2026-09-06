#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "store/user_repository.h"
#include "user/v1/user.grpc.pb.h"

namespace http_server_demo {

// UserService 的 gRPC 实现：业务逻辑通过 UserRepository 访问 MySQL(持久化)+Redis(缓存)
class UserServiceImpl final : public user::v1::UserService::Service {
 public:
  explicit UserServiceImpl(std::shared_ptr<UserRepository> repo);

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
  std::shared_ptr<UserRepository> repo_;
};

}  // namespace http_server_demo
