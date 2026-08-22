# http_server_demo 常用命令封装
# 用法：make <proto|build|run-grpc|run-gateway-1|run-gateway-2|run-nginx|stop-nginx|test|clean>

BUILD_DIR := build
GEN_DIR   := generated

.PHONY: all proto build clean \
        run-grpc run-gateway-1 run-gateway-2 run-nginx stop-nginx test

all: build

# 1) 重新生成 gRPC/protobuf 代码（修改 user.proto 后执行）
proto:
	mkdir -p $(GEN_DIR)
	protoc -I proto \
		--cpp_out=$(GEN_DIR) \
		--grpc_out=$(GEN_DIR) \
		--plugin=protoc-gen-grpc=$$(which grpc_cpp_plugin) \
		proto/user/v1/user.proto

# 2) 编译（自动先跑 proto 生成）
build: proto
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR) -j$$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

# 3) 启动业务服务（gRPC :9090，需先启动 redis）
run-grpc:
	./$(BUILD_DIR)/grpc_server --port=9090 --redis-host=127.0.0.1 --redis-port=6379

# 4) 启动两个 HTTP 网关实例（负载均衡后端）
run-gateway-1:
	./$(BUILD_DIR)/gateway --port=8081 --grpc-addr=127.0.0.1:9090 --id=gateway-1

run-gateway-2:
	./$(BUILD_DIR)/gateway --port=8082 --grpc-addr=127.0.0.1:9090 --id=gateway-2

# 5) 启动 nginx（HTTP :8080 -> 轮询两个网关）
run-nginx:
	mkdir -p deploy/nginx/logs
	nginx -c $(abspath deploy/nginx/nginx.conf) -p $(abspath deploy/nginx)

stop-nginx:
	-nginx -c $(abspath deploy/nginx/nginx.conf) -p $(abspath deploy/nginx) -s stop 2>/dev/null

# 6) 端到端自测（依赖 redis + grpc_server + gateway 已启动）
test:
	@echo "== 创建用户 =="
	@curl -s -X POST http://127.0.0.1:8080/api/v1/users -H 'Content-Type: application/json' -d '{"name":"tom","email":"tom@example.com"}'; echo
	@echo "== 查询用户 =="
	@curl -s http://127.0.0.1:8080/api/v1/users/u1; echo
	@echo "== 用户列表 =="
	@curl -s 'http://127.0.0.1:8080/api/v1/users?limit=10&offset=0'; echo
	@echo "== 统计 =="
	@curl -s http://127.0.0.1:8080/api/v1/stats; echo
	@echo "== 健康检查（观察 X-Gateway-Instance 头的变化即可看到负载均衡轮询）=="
	@for i in 1 2 3 4; do curl -s -i http://127.0.0.1:8080/healthz 2>/dev/null | grep -i 'x-gateway-instance'; done

clean:
	rm -rf $(BUILD_DIR)
