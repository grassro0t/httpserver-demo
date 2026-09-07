cmake_minimum_required(VERSION 3.18)
project(http_server_demo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 生成 compile_commands.json，供 IDE/clangd 索引第三方与内部 include
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ===================== 依赖发现 =====================
# HTTP 网关框架
find_package(Drogon CONFIG REQUIRED)
# JSON 库（header-only）
find_package(nlohmann_json CONFIG REQUIRED)
# gRPC / protobuf：通过 CMake config 定位（提供 gRPC::grpc++ / protobuf::libprotobuf）
# gRPC 的 CMake target 依赖 c-ares::cares，需先显式加载其 config
find_package(Protobuf CONFIG REQUIRED)
find_package(c-ares CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
# hiredis 官方未提供 CMake config，fallback 到 pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
# MySQL 客户端（libmysqlclient，官方无 CMake config）
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h REQUIRED)
find_library(MYSQL_CLIENT_LIB mysqlclient REQUIRED)

# ===================== 生成 gRPC/protobuf 代码 =====================
set(PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/user/v1/user.proto)
set(GEN_DIR ${CMAKE_SOURCE_DIR}/generated)
set(GEN_SRCS
    ${GEN_DIR}/user/v1/user.pb.cc
    ${GEN_DIR}/user/v1/user.grpc.pb.cc)
set(GEN_HDRS
    ${GEN_DIR}/user/v1/user.pb.h
    ${GEN_DIR}/user/v1/user.grpc.pb.h)

add_custom_command(
  OUTPUT ${GEN_SRCS} ${GEN_HDRS}
  COMMAND protoc
          -I ${CMAKE_SOURCE_DIR}/proto
          --cpp_out=${GEN_DIR}
          --grpc_out=${GEN_DIR}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
          ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Generating gRPC/protobuf code from user.proto"
  VERBATIM)

# ===================== grpc-server（业务服务） =====================
add_executable(grpc_server
  src/grpc_server/main.cpp
  src/grpc_server/user_service.cpp
  src/store/mysql_pool.cpp
  src/store/redis_client.cpp
  src/store/user_repository.cpp
  src/common/config.cpp
  ${GEN_SRCS})
target_link_libraries(grpc_server PRIVATE
  gRPC::grpc++
  protobuf::libprotobuf
  PkgConfig::HIREDIS
  ${MYSQL_CLIENT_LIB}
  nlohmann_json::nlohmann_json)
target_include_directories(grpc_server PRIVATE
  ${GEN_DIR}
  ${MYSQL_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src)

# ===================== gateway（HTTP 网关） =====================
add_executable(gateway
  src/gateway/main.cpp
  src/gateway/gateway_context.cpp
  src/gateway/grpc_client.cpp
  src/gateway/user_controller.cpp
  src/common/config.cpp
  ${GEN_SRCS})
target_link_libraries(gateway PRIVATE
  gRPC::grpc++
  protobuf::libprotobuf
  Drogon::Drogon
  nlohmann_json::nlohmann_json)
target_include_directories(gateway PRIVATE
  ${GEN_DIR}
  ${CMAKE_SOURCE_DIR}/src)
