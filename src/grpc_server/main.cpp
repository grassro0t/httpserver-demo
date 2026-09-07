// grpc-server 入口：启动 gRPC 业务服务
//   存储架构：MySQL(持久化,连接池) + Redis(缓存)
#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>

#include "common/config.h"
#include "grpc_server/user_service.h"
#include "store/mysql_pool.h"
#include "store/redis_client.h"
#include "store/user_repository.h"

using grpc::ServerBuilder;

int main(int argc, char** argv) {
  using namespace http_server_demo;

  auto args = ParseArgs(argc, argv);
  int port = std::stoi(GetArg(args, "port", "9090"));

  // MySQL 配置（持久层，走连接池）
  MysqlConfig mysql_cfg;
  mysql_cfg.host = GetArg(args, "mysql-host", "127.0.0.1");
  mysql_cfg.port = std::stoi(GetArg(args, "mysql-port", "3306"));
  mysql_cfg.user = GetArg(args, "mysql-user", "root");
  mysql_cfg.password = GetArg(args, "mysql-password", "");
  mysql_cfg.database = GetArg(args, "mysql-database", "http_server_demo");
  mysql_cfg.pool_size = std::stoi(GetArg(args, "mysql-pool-size", "8"));

  // Redis 配置（缓存层）
  std::string redis_host = GetArg(args, "redis-host", "127.0.0.1");
  int redis_port = std::stoi(GetArg(args, "redis-port", "6379"));

  // 1. 初始化 MySQL 连接池（自动建库建表）
  auto pool = std::make_shared<MysqlPool>();
  if (!pool->init(mysql_cfg)) {
    std::cerr << "[grpc-server] MySQL 连接池初始化失败: " << pool->lastError() << std::endl;
    return 1;
  }
  std::cout << "[grpc-server] MySQL 连接池就绪 " << mysql_cfg.host << ":" << mysql_cfg.port
            << " db=" << mysql_cfg.database << " 池大小=" << mysql_cfg.pool_size << std::endl;

  // 2. 连接 Redis（缓存）
  auto redis = std::make_shared<RedisClient>();
  if (!redis->connect(redis_host, redis_port)) {
    std::cerr << "[grpc-server] 连接 Redis 失败 " << redis_host << ":" << redis_port << " - "
              << redis->lastError() << std::endl;
    return 1;
  }
  std::cout << "[grpc-server] Redis 缓存已连接 " << redis_host << ":" << redis_port << std::endl;

  // 3. 组装数据访问层与业务服务
  auto repo = std::make_shared<UserRepository>(pool, redis);
  UserServiceImpl service(repo);

  // 4. 注册服务并启动 gRPC Server
  ServerBuilder builder;
  builder.AddListeningPort("0.0.0.0:" + std::to_string(port), grpc::InsecureServerCredentials());
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
