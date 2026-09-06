# 技术笔记：nginx + gRPC + MySQL + Redis 的 C++ HTTP 后端

> 记录 `http_server_demo` 的设计思路、关键技术点与开发踩坑，适合复习/教学。

## 1. 架构设计思路

分层微服务，各层职责单一：

```
客户端 ──HTTP──▶ nginx（流量入口 / 负载均衡）
                     │ 反向代理
                     ▼
              gateway（HTTP 网关：协议转换 REST→gRPC）
                     │ gRPC（二进制、强类型、HTTP/2）
                     ▼
              grpc-server（业务服务）
                     │ 双写双读
              ┌──────┴───────┐
              ▼              ▼
         MySQL（持久化）   Redis（缓存）
         连接池管理         Cache-Aside
```

- **nginx 负责流量层**：`upstream` 轮询分发，可平滑扩展网关实例。
- **gateway 负责协议层**：REST/JSON → gRPC 转换，客户端不感知内部服务。
- **grpc-server 负责业务层**：只依赖数据访问层，不暴露 HTTP。
- **MySQL 负责持久层**：数据唯一真源（source of truth），连接走连接池。
- **Redis 负责缓存层**：加速读，只存可重建数据，挂了不影响正确性。

## 2. 存储架构演进（重点）

### v1：Redis 单一存储（本项目初版）
用户 JSON 直接存 Redis：`user:{id}` + `users:ids`(Set) + `users:seq`(INCR)。

**问题**：
1. Redis 是内存库，数据易失（未持久化/故障切换会丢）——不适合当唯一存储。
2. 数据无法用 SQL 查询、关联、事务。
3. 单个 `redisContext` 被 gRPC 多线程共享，**线程不安全**且串行。

### v2：MySQL 持久化 + Redis 缓存（当前版）
- MySQL 表 `users` 存全量数据，`id BIGINT AUTO_INCREMENT`。
- Redis 只做缓存：读走 **Cache-Aside**，写时主动填充/失效。
- MySQL 连接引入**连接池**，解决并发下的连接复用与线程安全。

**收益**：数据安全（MySQL 落盘）+ 读性能（缓存命中绕过 DB）+ 并发正确（连接池）。
**代价**：引入缓存一致性问题（靠写时失效 + TTL 兜底，见 §5）。

## 3. MySQL C API（libmysqlclient）封装要点

### 3.1 为什么用 libmysqlclient 而非 mysql-connector-c++
- `mysql` 完整版自带 `libmysqlclient` + 头文件 + `mysql_config`，无需额外安装。
- 与 hiredis 封装风格一致：**C API + RAII 壳**，能自己掌控连接生命周期（这正是连接池需要的）。
- `mysql-connector-c++` 的 JDBC 风格驱动本身不带连接池。

### 3.2 CMake 集成

```cmake
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h REQUIRED)
find_library(MYSQL_CLIENT_LIB mysqlclient REQUIRED)
target_link_libraries(grpc_server PRIVATE ${MYSQL_CLIENT_LIB})
target_include_directories(grpc_server PRIVATE ${MYSQL_INCLUDE_DIR})
```

> Homebrew 版会把 `mysql.h` 放在 `/opt/homebrew/include/mysql/`，代码里 `#include <mysql/mysql.h>`。

### 3.3 关键 API 使用

```cpp
MYSQL* m = mysql_init(nullptr);
mysql_real_connect(m, host, user, pass, db, port, nullptr, 0);  // db 传 nullptr 可先不选库
mysql_set_character_set(m, "utf8mb4");                          // 务必设字符集

// 非查询：INSERT/UPDATE/DELETE/CREATE —— mysql_query 即可
mysql_query(m, sql);

// 查询：必须 mysql_store_result 取走结果，否则下一个命令报 "Commands out of sync"
MYSQL_RES* res = mysql_store_result(m);
while ((row = mysql_fetch_row(res))) { /* row[i] 是 const char*，NULL 表示 NULL */ }
mysql_free_result(res);  // 用 RAII 自动释放

mysql_insert_id(m);              // 上一条 INSERT 的自增主键
mysql_real_escape_string(m, ...) // 防 SQL 注入
```

**RAII 封装**：
- `MysqlConn`：持有 `MYSQL*`，析构 `mysql_close`。
- `MysqlResult`：持有 `MYSQL_RES*`，析构 `mysql_free_result`。
- `escape()`：所有字符串字段拼 SQL 前必须转义（防注入）。

## 4. MySQL 连接池设计（核心模块）

### 4.1 为什么需要
- 每个请求新建 MySQL 连接开销大（TCP + 握手 + 认证）。
- 单连接被多线程共享：`MYSQL*` **非线程安全**，命令会交错。
- 连接池 = 固定 N 个连接，线程互斥借用/归还，天然线程安全 + 复用。

### 4.2 实现要点

```cpp
class MysqlPool {
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<MysqlConn*> idle_;                    // 空闲连接栈
  std::vector<std::unique_ptr<MysqlConn>> conns_;   // 持有所有权

  Guard acquire() {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [this] { return !idle_.empty(); });  // 池空则阻塞等待
    auto* c = idle_.back(); idle_.pop_back();
    return Guard(this, c);
  }
  void release(MysqlConn* c) { /* push_back + notify_one */ }
};
```

- **RAII 守卫**：`Guard` 析构自动归还，业务代码异常安全（`pool_->acquire()` 返回 guard，作用域结束即归还）。
- **建池时机**：启动时 `init()` 一次性建好全部连接（含失败重试语义），首次连接自动 `CREATE DATABASE/TABLE IF NOT EXISTS`。

### 4.3 边界与扩展
- 池空等待 vs 动态扩容：demo 用阻塞等待（简单）；生产可用超时 + 扩容。
- 连接失效（MySQL 重启后连接断开）：可取用时 `mysql_ping()` 探活重建。
- 高级实现可换 thread_local 连接（无锁）或第三方池（如 mysqlx::SessionPool）。

## 5. Cache-Aside 缓存模式与失效策略

### 5.1 读路径（先缓存，未命中回源并回填）

```
GetUser(id)
  ├─ 命中 → Redis GET user:{id} → 返回
  └─ 未命中 → MySQL SELECT → 回填 Redis user:{id} (TTL 60s) → 返回
```

### 5.2 写路径（先 DB，再写缓存/失效）

```
CreateUser(...)
  1) INSERT MySQL（拿自增 id）
  2) SET Redis user:{id}（主动填充）
  3) 失效列表缓存（删 cache:users:list 索引下所有 key）
  4) DEL stats:total_users
```

### 5.3 列表缓存如何批量失效
Redis 不支持通配 `DEL`，因此写缓存时把 key 记入一个 Set：

```
写列表缓存:  SET users:list:10:0 {...} EX 10 ; SADD cache:users:list users:list:10:0
新用户写入:  SMEMBERS cache:users:list → 逐个 DEL → DEL cache:users:list
```

### 5.4 一致性权衡
- **写时失效 + 短 TTL 兜底**：最多在 TTL 窗口内读到旧列表/统计（10~30s），可接受。
- 极端并发（删缓存与写 DB 交错）可能导致短暂脏读——进阶可用「延迟双删」或消息队列。

## 6. nginx 负载均衡

（沿用原设计）`upstream` 默认轮询：

```nginx
upstream gateway_backend {
    server 127.0.0.1:8081;
    server 127.0.0.1:8082;   # 可加 weight / max_fails / backup
}
location / { proxy_pass http://gateway_backend; ... }
```

- `least_conn`：按连接数分发；`ip_hash`：会话粘滞。
- 每个 gateway 响应带 `X-Gateway-Instance` 头，便于观察轮询。
- nginx 也能负载均衡 gRPC（`grpc_pass`），适合 gRPC 直接对外的场景。
- 本机用 `nginx -c conf -p deploy/nginx` 运行，日志写项目本地，避免权限问题。

## 7. gRPC C++ 集成

- 安装选 **brew**（自带 `protoc` / `grpc_cpp_plugin` / `libgrpc++`），避免 CMake FetchContent 编译 20~40 分钟。
- 代码生成：

```bash
protoc -I proto \
  --cpp_out=generated \                     # user.pb.{h,cc}（消息）
  --grpc_out=generated \                    # user.grpc.pb.{h,cc}（服务骨架）
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  proto/user/v1/user.proto
```

- CMake：`find_package(PkgConfig)` + `pkg_check_modules(GRPC/PROTOBUF ... IMPORTED_TARGET)`；生成代码通过 `add_custom_command` 接入构建，且提交进仓库保证开箱即用。
- 服务实现：继承 `user::v1::UserService::Service`，返回 `grpc::Status`；状态码贯穿（`NOT_FOUND` → 网关映射 404）。

## 8. Drogon 1.9 使用要点

- 控制器：`class X : public drogon::HttpController<X>` + `METHOD_LIST_BEGIN/ADD_METHOD_TO/METHOD_LIST_END`，Drogon 静态注册自动发现。
- 参数 API（1.9 版）：
  - 路径参数 `{id}` → `req->getRoutingParameters()`（`std::vector`，按模板位置）
  - 查询参数 → `req->getOptionalParameter<int>("limit")`
  - JSON body → `req->getJsonObject()`（jsoncpp 的 Json::Value）
- 响应：`drogon::HttpResponse::newHttpResponse()` + `setBody(json字符串)` + `addHeader("X-Gateway-Instance", id)`。
- 业务 JSON 用 nlohmann/json（与存储层一致），Drogon 仅做 HTTP 传输，两者以字符串 body 衔接。

## 9. hiredis 封装（缓存客户端）

- hiredis 是 C API，`redisCommand` 返回 `void*`，需包一层：

```cpp
redisReply* RedisClient::Command(const char* fmt, ...) {
  va_list args; va_start(args, fmt);
  redisReply* reply = static_cast<redisReply*>(redisvCommand(ctx_, fmt, args));
  va_end(args);
  return reply;
}
```

- `RedisReply` RAII 自动 `freeReplyObject`；注意 reply 类型检查（`REDIS_REPLY_INTEGER/STRING/NIL/ARRAY`）。
- hiredis 的 context 非线程安全——当前仅 grpc-server 主路径经 repository 单线程使用 RedisClient 逻辑调用；若扩多线程需连接池或 thread_local。

## 10. 踩坑记录（开发实录）

1. **protobuf 版本错配**：`brew install mysql` 触发 protobuf 35.1→36.1 升级，旧 `grpc_cpp_plugin` 还链接 35.1 库导致崩溃（signal 6），且 CMake 缓存旧生成代码报 "incompatible version"。解决：`brew upgrade grpc` 对齐后 `rm -rf build` 全量重建。
2. **CMake 缓存生成代码**：`make proto` 重生成后增量编译仍报版本错，需清理 build 全量编译（或确认生成文件时间戳）。
3. **MySQL root 认证**：brew MySQL root 默认空密码、支持 TCP（127.0.0.1）连接；程序默认参数与之匹配。
4. **`mysql_query` 后必须消费结果集**：SELECT 不 `mysql_store_result` 会导致下一条命令 "Commands out of sync"。
5. **字符集**：连接后必须 `mysql_set_character_set(m, "utf8mb4")`，否则中文乱码。
6. **hiredis 返回 `void*` / API 名**：`redisCommand` 返回 `void*`；变参命令是 `redisvCommand`（无 `redisCommandv`）。
7. **Drogon 1.9 无 `getPathParameter`**：路径参数走 `getRoutingParameters()`，查询参数用 `getOptionalParameter<T>()`。
8. **CMake 链接漏源文件**：`grpc_server` 目标漏加某 `.cpp` 会报 undefined symbol；源文件列表需逐一对齐。
9. **自增 ID 从 1 开始**：改造后用户 ID 从 `"u1"` 变成数字 `"1"`，测试脚本需动态提取 ID 而非硬编码。
10. **std::cout 全缓冲**：日志重定向到文件后用 `std::endl` 保证即时 flush。

## 11. 可扩展方向

- 连接池增强：`mysql_ping` 探活、获取超时、动态扩容。
- 缓存进阶：延迟双删、`SSCAN` 分页列表、布隆过滤器防缓存穿透。
- grpc-server 侧 gRPC 异步 API / Drogon 协程，释放阻塞线程。
- MySQL 读写分离（主从 + 多数据源）、分库分表。
- OpenTelemetry 分布式追踪贯穿 nginx/gateway/service/DB 全链路。

