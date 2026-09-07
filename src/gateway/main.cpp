// gateway 入口：启动 Drogon HTTP 服务器（负载均衡中的一个后端实例）
#include <drogon/drogon.h>

#include <iostream>

#include "common/config.h"
#include "gateway/gateway_context.h"
#include "gateway/user_controller.h"

int main(int argc, char** argv) {
  using namespace http_server_demo;

  auto args = ParseArgs(argc, argv);
  GatewayConfig cfg;
  cfg.port = std::stoi(GetArg(args, "port", "8080"));
  cfg.grpc_addr = GetArg(args, "grpc-addr", "127.0.0.1:9090");
  cfg.instance_id = GetArg(args, "id", "gateway-unknown");

  GatewayContext::instance().init(cfg);

  std::cout << "[gateway] 实例 " << cfg.instance_id << " 监听 0.0.0.0:" << cfg.port
            << "，gRPC 后端 " << cfg.grpc_addr << std::endl;

  drogon::app().addListener("0.0.0.0", cfg.port).setLogLevel(trantor::Logger::kWarn).run();

  return 0;
}
