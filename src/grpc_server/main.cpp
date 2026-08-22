// grpc-server 入口：启动 gRPC 业务服务，连接 Redis
#include <iostream>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "common/config.h"
#include "grpc_server/user_service.h"
#include "store/redis_client.h"

using grpc::ServerBuilder;

int main(int argc, char** argv) {
  using namespace http_server_demo;

  auto args = ParseArgs(argc, argv);
  int port = std::stoi(GetArg(args, "port", "9090"));
  std::string redis_host = GetArg(args, "redis-host", "127.0.0.1");
  int redis_port = std::stoi(GetArg(args, "redis-port", "6379"));

  // 1. 连接 Redis
  auto redis = std::make_shared<RedisClient>();
  if (!redis->connect(redis_host, redis_port)) {
    std::cerr << "[grpc-server] 连接 Redis 失败 " << redis_host << ":" << redis_port
              << " - " << redis->lastError() << std::endl;
    return 1;
  }
  std::cout << "[grpc-server] Redis 已连接 " << redis_host << ":" << redis_port << std::endl;

  // 2. 注册服务并启动 gRPC Server
  UserServiceImpl service(redis);
  ServerBuilder builder;
  builder.AddListeningPort("0.0.0.0:" + std::to_string(port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    std::cerr << "[grpc-server] gRPC Server 启动失败" << std::endl;
    return 1;
  }
  std::cout << "[grpc-server] UserService 监听 0.0.0.0:" << port << std::endl;

  server->Wait();
  return 0;
}
