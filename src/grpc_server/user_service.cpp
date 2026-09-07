#include "grpc_server/user_service.h"

#include <utility>
#include <vector>

namespace http_server_demo {

UserServiceImpl::UserServiceImpl(std::shared_ptr<UserRepository> repo) : repo_(std::move(repo)) {}

grpc::Status UserServiceImpl::CreateUser(grpc::ServerContext* context,
                                         const user::v1::CreateUserRequest* request,
                                         user::v1::CreateUserReply* reply) {
  repo_->CountRequest();

  if (request->name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "name is required");
  }

  user::v1::User user;
  if (!repo_->CreateUser(request->name(), request->email(), &user)) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "create user failed");
  }

  *reply->mutable_user() = std::move(user);
  return grpc::Status::OK;
}

grpc::Status UserServiceImpl::GetUser(grpc::ServerContext* context,
                                      const user::v1::GetUserRequest* request,
                                      user::v1::GetUserReply* reply) {
  repo_->CountRequest();

  if (request->id().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "id is required");
  }

  auto user = repo_->GetUser(request->id());
  if (!user) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "user not found");
  }

  *reply->mutable_user() = std::move(*user);
  return grpc::Status::OK;
}

grpc::Status UserServiceImpl::ListUsers(grpc::ServerContext* context,
                                        const user::v1::ListUsersRequest* request,
                                        user::v1::ListUsersReply* reply) {
  repo_->CountRequest();

  int limit = request->limit() > 0 ? request->limit() : 10;
  int offset = request->offset() > 0 ? request->offset() : 0;

  std::vector<user::v1::User> users;
  int64_t total = 0;
  if (!repo_->ListUsers(limit, offset, &users, &total)) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "list users failed");
  }

  reply->set_total(total);
  for (auto& u : users) {
    *reply->add_users() = std::move(u);
  }
  return grpc::Status::OK;
}

grpc::Status UserServiceImpl::GetStats(grpc::ServerContext* context,
                                       const user::v1::GetStatsRequest* request,
                                       user::v1::GetStatsReply* reply) {
  int64_t total_users = 0;
  int64_t request_count = 0;
  if (!repo_->GetStats(&total_users, &request_count)) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "get stats failed");
  }

  reply->set_total_users(total_users);
  reply->set_request_count(request_count);
  return grpc::Status::OK;
}

}  // namespace http_server_demo
