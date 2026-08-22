# 技术笔记：nginx + gRPC + Redis 的 C++ HTTP 后端

> 本文记录 `http_server_demo` 的设计思路、关键技术点与开发过程中踩过的坑，适合作为复习/教学材料。

## 1. 架构设计思路

分层微服务，三层职责单一：

```
客户端 ──HTTP──▶ nginx（流量入口 / 负载均衡）
                     │ 反向代理
                     ▼
              gateway（HTTP 网关：协议转换 REST→gRPC）
                     │ gRPC（二进制、强类型、HTTP/2）
                     ▼
              grpc-server（业务服务：业务逻辑 + 存储）
                     │ 协议（RESP）
                     ▼
              Redis（存储层）
```

- **nginx 负责流量层**：`upstream` 维护后端实例池，`round-robin` 轮询分发；后续可平滑扩展实例、加权重、健康检查。
- **gateway 负责协议层**：对外是 REST/JSON，对内是 gRPC。客户端不直接感知内部服务。
- **grpc-server 负责业务层**：只关心业务与存储，不暴露 HTTP。
- **Redis 负责存储层**：无状态服务共享同一份数据。

这种结构的好处：网关与业务服务可以独立扩缩容；客户端协议变了只改网关；业务层内部 RPC 享受 gRPC 的性能与类型安全。

## 2. nginx 负载均衡

### 2.1 配置要点

```nginx
upstream gateway_backend {
    server 127.0.0.1:8081;   # gateway-1
    server 127.0.0.1:8082;   # gateway-2
}

server {
    listen 8080;
    location / {
        proxy_pass http://gateway_backend;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

- 默认策略是**轮询（round-robin）**：请求按顺序分发给各后端，均摊流量。
- 其他策略：
  - `least_conn`：发给当前连接数最少的后端，适合长短请求混合场景。
  - `ip_hash`：同一客户端 IP 固定打到同一后端，适合需要会话粘滞的场景（也可用 `sticky` 模块）。
  - 后端可加权重：`server 127.0.0.1:8081 weight=3;`
  - 可加 `max_fails=2 fail_timeout=10s` 做失败摘除。
- `proxy_set_header` 用于透传真实客户端信息，后端才能拿到正确的 IP。
- 本 demo 每个 gateway 响应带 `X-Gateway-Instance` 头，方便观察轮询是否生效。

### 2.2 本机运行方式

`make run-nginx` 实际执行：

```bash
nginx -c $(pwd)/deploy/nginx/nginx.conf -p $(pwd)/deploy/nginx
```

- `-c` 指定配置文件；`-p` 指定运行前缀目录（日志、pid 文件写本地 `deploy/nginx/logs/`，避免写全局 `/opt/homebrew/var/` 的权限问题）。
- 停止用 `make stop-nginx`。

> 提示：nginx 也能做 **gRPC 负载均衡**（`grpc_pass`），适用于 gRPC 服务直接对外的场景；本 demo 的 gRPC 是内网服务，由 gateway 直连，故负载均衡只针对 HTTP 层。

## 3. gRPC C++ 集成

### 3.1 依赖安装方式对比

| 方式 | 优点 | 缺点 |
|---|---|---|
| `brew install grpc` | 编译好的 bottle，秒装；自带 `protoc`、`grpc_cpp_plugin`、`libgrpc++` | 版本跟随 brew |
| CMake `FetchContent` 拉源码 | 版本锁定灵活 | **编译 gRPC 本体需 20~40 分钟** |

demo 选 brew 方案，构建脚本省心得多。

### 3.2 代码生成

```bash
protoc -I proto \
  --cpp_out=generated \
  --grpc_out=generated \
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  proto/user/v1/user.proto
```

- `--cpp_out` 生成消息代码 `user.pb.{h,cc}`
- `--grpc_out` + `grpc_cpp_plugin` 生成服务骨架 `user.grpc.pb.{h,cc}`
- package `user.v1` 会映射为 C++ 命名空间 `user::v1`

### 3.3 CMake 集成（brew 场景）

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
```

- Homebrew 安装的 gRPC/protobuf/hiredis 都会带 `.pc` 文件，`pkg-config` 是定位它们最省事的方式（比手写 include/lib 路径可靠）。
- 生成代码的编译用 `add_custom_command` 接入构建，保证 `user.proto` 变化时自动重生成，且生成目录提交进仓库（开箱即用）。

### 3.4 服务实现

继承 protoc 生成的 `user::v1::UserService::Service`，重写返回 `grpc::Status` 的虚函数：

```cpp
grpc::Status UserServiceImpl::GetUser(grpc::ServerContext*, const GetUserRequest* req, GetUserReply* reply) {
  // ...业务...
  if (!found) return grpc::Status(grpc::StatusCode::NOT_FOUND, "user not found");
  return grpc::Status::OK;
}
```

状态码贯穿全局：业务服务返回 `NOT_FOUND` → 网关 client 识别为 `kNotFound` → HTTP 映射为 404。

## 4. hiredis 封装

hiredis 是 Redis 官方 C 客户端，**没有 C++ 版本**，因此封装了 RAII 壳：

### 4.1 关键坑：`redisCommand` 返回 `void *`

hiredis 为 C 兼容把命令函数声明为返回 `void *`，直接用会编译报错。封装为：

```cpp
redisReply* RedisClient::Command(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  redisReply* reply = static_cast<redisReply*>(redisvCommand(ctx_, fmt, args));
  va_end(args);
  return reply;
}
```

- 变参命令使用 **`redisvCommand`**（注意 API 名，不是 `redisCommandv`）。

### 4.2 RAII 管理 reply

`redisReply` 必须用 `freeReplyObject()` 释放。用 `RedisReply` 封装后自动释放：

```cpp
class RedisReply {
 public:
  explicit RedisReply(redisReply* reply) : reply_(reply) {}
  ~RedisReply() { if (reply_) freeReplyObject(reply_); }
  // 禁用拷贝，只允许局部变量使用
};
```

### 4.3 reply 类型检查

hiredis 的 reply 是联合体式结构，按 `type` 区分：

```cpp
if (r.get()->type == REDIS_REPLY_INTEGER) return r.get()->integer;  // INCR/SCARD
if (r.get()->type == REDIS_REPLY_NIL)      return std::nullopt;      // GET 未命中
if (r.get()->type == REDIS_REPLY_ARRAY)    // SMEMBERS：遍历 element[i]
```

## 5. Drogon 1.9 使用要点

### 5.1 控制器注册

```cpp
class UserController : public drogon::HttpController<UserController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(UserController::getUser, "/api/v1/users/{id}", drogon::Get);
  METHOD_LIST_END
  // handler 签名固定
  void getUser(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};
```

Drogon 通过静态注册自动发现控制器，`app().run()` 即生效，无需手动注册路由。

### 5.2 请求参数 API（1.9 版）

| 需求 | API |
|---|---|
| 路径参数 `{id}` | `req->getRoutingParameters()` → `std::vector<std::string>`（按模板位置） |
| 查询参数 | `req->getOptionalParameter<int>("limit")` → `std::optional<int>` |
| JSON body | `req->getJsonObject()` → `std::shared_ptr<Json::Value>` |

> **踩坑**：Drogon 1.9 **没有** `getPathParameter`，也**没有** `getOptionalQueryParameter`（编译直接报错）。路径参数统一走 `getRoutingParameters()`。

### 5.3 响应构造

```cpp
auto resp = drogon::HttpResponse::newHttpResponse();
resp->setStatusCode(drogon::k201Created);
resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
resp->setBody(json_string);                       // 由 nlohmann dump 而来
resp->addHeader("X-Gateway-Instance", instance);  // 负载均衡观察用
callback(resp);
```

### 5.4 框架约定

- 业务 JSON 用 **nlohmann/json**（跨 gateway/grpc-server 共享同一序列化），Drogon 只负责 HTTP 传输，两者通过字符串 body 衔接，职责清晰。
- Drogon 是异步框架，handler 用回调返回响应，内部 gRPC 同步调用简单直白（demo 够用；高并发可换 Drogon 协程 + gRPC async）。

## 6. Redis 数据建模

| Key | 类型 | 用途 |
|---|---|---|
| `user:{id}` | String | 用户 JSON（`SET`/`GET`） |
| `users:ids` | Set | ID 索引，`SADD`/`SMEMBERS` 支持列表与总数 |
| `users:seq` | String(Int) | `INCR` 生成自增用户 ID（演示原子操作） |
| `stats:request_count` | String(Int) | `INCR` 请求计数（演示原子操作） |

设计取舍：

- **JSON 整体存取**：简单直观，适合 demo；字段多时可拆 hash（`HSET user:{id} name ...`）。
- **Set 做索引**：`SMEMBERS` 拿到全部 ID 再批量 `GET`；数据量大时应改 `SSCAN` 分页或换 Lua/范围分页。
- **INCR 自增 ID**：仅演示；生产环境 ID 生成更常用 UUID 或分布式发号器。
- **过期/缓存**：demo 未加 TTL，生产可给热点键设 `EXPIRE`。

## 7. 踩坑记录（本次开发实录）

1. **hiredis `redisCommand` 返回 `void*`** → 编译报 `cannot convert void*`。解决：封装 `Command()`，内部 `redisvCommand` + `static_cast<redisReply*>`。
2. **hiredis 变参 API 名**：是 `redisvCommand`，不存在 `redisCommandv`。
3. **Drogon 1.9 无 `getPathParameter`**：路径参数用 `getRoutingParameters()`（vector），查询参数用 `getOptionalParameter<T>()`。
4. **`getParameter()` 只覆盖查询/表单参数**：取不到 `{id}` 路径参数，需区分。
5. **`std::strtoll` 未声明**：需显式 `#include <cstdlib>`（跨编译器依赖隐性传递不可靠）。
6. **CMake 链接漏源文件**：`grpc_server` 目标漏加 `src/common/config.cpp`，链接报 `ParseArgs/GetArg` undefined。目标源文件列表要逐一对齐。
7. **`pkg-config` 未装**：`find_package(PkgConfig)` 会失败。`brew install pkg-config`（实为 pkgconf）。
8. **nginx 日志写全局目录权限问题**：用 `-p` 指定前缀目录，日志/pid 落到项目本地 `deploy/nginx/logs/`。
9. **std::cout 重定向到文件是全缓冲**：`nohup ... > log &` 后日志可能不可见，用 `std::endl` 保证即时 flush（本 demo 已处理）。

## 8. 可扩展方向

- gateway 增加**协程 + gRPC 异步调用**，提升并发吞吐。
- nginx 层加 **gRPC 负载均衡**（`grpc_pass`）或 `least_conn`/健康检查。
- Redis 层改 **连接池**（hiredis 每次连接单独建立，可池化）或换 redis-plus-plus。
- 数据模型换 **hash** 结构、加 TTL 缓存、`SSCAN` 分页。
- 加 **grpc-gateway（google api）** 用注解直接从 proto 生成 REST，减少手写映射。
- 引入 **分布式追踪**（OpenTelemetry）跨三层看调用链。
