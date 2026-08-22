#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpTypes.h>

namespace http_server_demo {

// Drogon HTTP 控制器：对外 REST API，内部经 gRPC 调用业务服务
class UserController : public drogon::HttpController<UserController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(UserController::createUser, "/api/v1/users", drogon::Post);
  ADD_METHOD_TO(UserController::getUser, "/api/v1/users/{id}", drogon::Get);
  ADD_METHOD_TO(UserController::listUsers, "/api/v1/users", drogon::Get);
  ADD_METHOD_TO(UserController::getStats, "/api/v1/stats", drogon::Get);
  ADD_METHOD_TO(UserController::health, "/healthz", drogon::Get);
  METHOD_LIST_END

  void createUser(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback);
  void getUser(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);
  void listUsers(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback);
  void getStats(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
  void health(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

}  // namespace http_server_demo
