#pragma once

#include <string>

#include "gateway/grpc_client.h"

namespace http_server_demo {

struct GatewayConfig {
  int port = 8080;
  std::string grpc_addr = "127.0.0.1:9090";
  std::string instance_id = "gateway-unknown";
};

// 网关全局上下文（单例）：持有配置与 gRPC client，
// 供 Drogon 控制器在各 handler 中访问。
class GatewayContext {
 public:
  static GatewayContext& instance();

  void init(const GatewayConfig& cfg) {
    cfg_ = cfg;
    grpc_.connect(cfg_.grpc_addr);
  }

  GrpcClient& grpc() { return grpc_; }
  const std::string& instanceId() const { return cfg_.instance_id; }
  const std::string& grpcAddr() const { return cfg_.grpc_addr; }

 private:
  GatewayContext() = default;
  GatewayConfig cfg_;
  GrpcClient grpc_;
};

}  // namespace http_server_demo
