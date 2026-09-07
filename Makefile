# http_server_demo 常用命令封装
# 用法：make <proto|build|run-grpc|run-gateway-1|run-gateway-2|run-nginx|stop-nginx|test|lint|format|clean>

BUILD_DIR := build
GEN_DIR   := generated
SRC_FILES := $(shell find src -name '*.cpp' -o -name '*.h' | sort)

.PHONY: all proto build clean \
        run-grpc run-gateway-1 run-gateway-2 run-nginx stop-nginx test lint format

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

# 3) 启动业务服务（gRPC :9090，需先启动 mysql + redis；可带 mysql 参数覆盖默认 root@127.0.0.1 空密码）
run-grpc:
	./$(BUILD_DIR)/grpc_server --port=9090 \
		--mysql-host=127.0.0.1 --mysql-port=3306 \
		--mysql-user=root --mysql-password= --mysql-pool-size=8 \
		--redis-host=127.0.0.1 --redis-port=6379

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

# 6) 端到端自测（依赖 mysql + redis + grpc_server + gateway + nginx 已启动）
test:
	@echo "== 创建用户 =="
	@curl -s -X POST http://127.0.0.1:8080/api/v1/users -H 'Content-Type: application/json' -d '{"name":"tom","email":"tom@example.com"}'; echo
	@echo "== 再建一个并回查（动态取自增 ID）=="
	@ID=$$(curl -s -X POST http://127.0.0.1:8080/api/v1/users -H 'Content-Type: application/json' -d '{"name":"jerry","email":"jerry@example.com"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p'); echo "jerry id=$$ID"; curl -s http://127.0.0.1:8080/api/v1/users/$$ID; echo
	@echo "== 用户列表 =="
	@curl -s 'http://127.0.0.1:8080/api/v1/users?limit=10&offset=0'; echo
	@echo "== 统计 =="
	@curl -s http://127.0.0.1:8080/api/v1/stats; echo
	@echo "== 健康检查（观察 X-Gateway-Instance 头变化看负载均衡轮询）=="
	@for i in 1 2 3 4; do curl -s -i http://127.0.0.1:8080/healthz 2>/dev/null | grep -i 'x-gateway-instance'; done

# 7) clang-format 代码风格检查（Google 风格，列宽 100，符合则退出码 0）
lint:
	@for f in $(SRC_FILES); do \
		clang-format --dry-run --Werror $$f >/dev/null 2>&1 || { echo "✗ 需格式化: $$f"; exit 1; }; \
	done; echo "✓ 全部源码符合 .clang-format (Google)";

# 一键将源码格式化为 Google 风格
format:
	clang-format -i $(SRC_FILES)

clean:
	rm -rf $(BUILD_DIR)
