#include "gateway/grpc_client.h"

#include <chrono>

namespace http_server_demo {

namespace {

constexpr std::chrono::seconds kRpcTimeout = std::chrono::seconds(3);

}  // namespace

bool GrpcClient::connect(const std::string& server_addr) {
  auto channel = grpc::CreateChannel(server_addr, grpc::InsecureChannelCredentials());
  stub_ = user::v1::UserService::NewStub(channel);
  return stub_ != nullptr;
}

RpcStatus GrpcClient::CreateUser(const std::string& name, const std::string& email,
                                 user::v1::User* out) {
  user::v1::CreateUserRequest request;
  user::v1::CreateUserReply reply;
  request.set_name(name);
  request.set_email(email);

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
  grpc::Status s = stub_->CreateUser(&ctx, request, &reply);
  if (s.ok()) {
    *out = reply.user();
    return RpcStatus::kOk;
  }
  err_ = s.error_message();
  return RpcStatus::kError;
}

RpcStatus GrpcClient::GetUser(const std::string& id, user::v1::User* out) {
  user::v1::GetUserRequest request;
  user::v1::GetUserReply reply;
  request.set_id(id);

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
  grpc::Status s = stub_->GetUser(&ctx, request, &reply);
  if (s.ok()) {
    *out = reply.user();
    return RpcStatus::kOk;
  }
  err_ = s.error_message();
  if (s.error_code() == grpc::StatusCode::NOT_FOUND) return RpcStatus::kNotFound;
  return RpcStatus::kError;
}

RpcStatus GrpcClient::ListUsers(int limit, int offset,
                                std::vector<user::v1::User>* out, int64_t* total) {
  user::v1::ListUsersRequest request;
  user::v1::ListUsersReply reply;
  request.set_limit(limit);
  request.set_offset(offset);

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
  grpc::Status s = stub_->ListUsers(&ctx, request, &reply);
  if (!s.ok()) {
    err_ = s.error_message();
    return RpcStatus::kError;
  }
  out->clear();
  for (const auto& u : reply.users()) {
    out->push_back(u);
  }
  *total = reply.total();
  return RpcStatus::kOk;
}

RpcStatus GrpcClient::GetStats(int64_t* total_users, int64_t* request_count) {
  user::v1::GetStatsRequest request;
  user::v1::GetStatsReply reply;

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
  grpc::Status s = stub_->GetStats(&ctx, request, &reply);
  if (!s.ok()) {
    err_ = s.error_message();
    return RpcStatus::kError;
  }
  *total_users = reply.total_users();
  *request_count = reply.request_count();
  return RpcStatus::kOk;
}

}  // namespace http_server_demo
