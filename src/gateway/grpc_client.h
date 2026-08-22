#pragma once

#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "user/v1/user.grpc.pb.h"

namespace http_server_demo {

// gRPC 调用结果分类，便于网关层映射 HTTP 状态码
enum class RpcStatus { kOk, kNotFound, kError };

// 封装 grpc::UserService 的同步 stub，屏蔽 gRPC 细节
class GrpcClient {
 public:
  GrpcClient() = default;
  ~GrpcClient() = default;

  GrpcClient(const GrpcClient&) = delete;
  GrpcClient& operator=(const GrpcClient&) = delete;

  bool connect(const std::string& server_addr);

  RpcStatus CreateUser(const std::string& name, const std::string& email,
                       user::v1::User* out);
  RpcStatus GetUser(const std::string& id, user::v1::User* out);
  RpcStatus ListUsers(int limit, int offset, std::vector<user::v1::User>* out,
                      int64_t* total);
  RpcStatus GetStats(int64_t* total_users, int64_t* request_count);

  const std::string& lastError() const { return err_; }

 private:
  std::unique_ptr<user::v1::UserService::Stub> stub_;
  std::string err_;
};

}  // namespace http_server_demo
