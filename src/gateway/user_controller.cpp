#include "gateway/user_controller.h"

#include <nlohmann/json.hpp>
#include <string>

#include "common/json_utils.h"
#include "gateway/gateway_context.h"

namespace http_server_demo {

namespace {

// 公共响应外壳：注入实例标识头 + 统一 JSON 返回
drogon::HttpResponsePtr MakeJsonResponse(drogon::HttpStatusCode code, const std::string& body) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(code);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->addHeader("X-Gateway-Instance", GatewayContext::instance().instanceId());
  resp->setBody(body);
  return resp;
}

drogon::HttpResponsePtr ErrorResponse(drogon::HttpStatusCode code, const std::string& msg) {
  nlohmann::json j;
  j["error"] = msg;
  return MakeJsonResponse(code, j.dump());
}

}  // namespace

void UserController::createUser(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  auto body = req->getJsonObject();
  if (!body || !body->isObject() || !body->isMember("name")) {
    callback(ErrorResponse(drogon::k400BadRequest, "name is required"));
    return;
  }

  user::v1::User user;
  RpcStatus st = GatewayContext::instance().grpc().CreateUser((*body)["name"].asString(),
                                                              (*body)["email"].asString(), &user);
  if (st != RpcStatus::kOk) {
    callback(
        ErrorResponse(drogon::k502BadGateway,
                      "grpc CreateUser failed: " + GatewayContext::instance().grpc().lastError()));
    return;
  }
  callback(MakeJsonResponse(drogon::k201Created, UserToJsonString(user)));
}

void UserController::getUser(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  // Drogon 1.9：路径参数通过 getRoutingParameters() 按模板位置获取
  const auto& params = req->getRoutingParameters();
  if (params.empty()) {
    callback(ErrorResponse(drogon::k400BadRequest, "id is required"));
    return;
  }
  std::string id = params[0];

  user::v1::User user;
  RpcStatus st = GatewayContext::instance().grpc().GetUser(id, &user);
  if (st == RpcStatus::kNotFound) {
    callback(ErrorResponse(drogon::k404NotFound, "user not found"));
    return;
  }
  if (st != RpcStatus::kOk) {
    callback(
        ErrorResponse(drogon::k502BadGateway,
                      "grpc GetUser failed: " + GatewayContext::instance().grpc().lastError()));
    return;
  }
  callback(MakeJsonResponse(drogon::k200OK, UserToJsonString(user)));
}

void UserController::listUsers(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  int limit = req->getOptionalParameter<int>("limit").value_or(10);
  int offset = req->getOptionalParameter<int>("offset").value_or(0);

  std::vector<user::v1::User> users;
  int64_t total = 0;
  RpcStatus st = GatewayContext::instance().grpc().ListUsers(limit, offset, &users, &total);
  if (st != RpcStatus::kOk) {
    callback(
        ErrorResponse(drogon::k502BadGateway,
                      "grpc ListUsers failed: " + GatewayContext::instance().grpc().lastError()));
    return;
  }

  nlohmann::json j;
  j["total"] = total;
  j["users"] = nlohmann::json::array();
  for (const auto& u : users) {
    j["users"].push_back(UserToJson(u));
  }
  callback(MakeJsonResponse(drogon::k200OK, j.dump()));
}

void UserController::getStats(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  int64_t total_users = 0;
  int64_t request_count = 0;
  RpcStatus st = GatewayContext::instance().grpc().GetStats(&total_users, &request_count);
  if (st != RpcStatus::kOk) {
    callback(
        ErrorResponse(drogon::k502BadGateway,
                      "grpc GetStats failed: " + GatewayContext::instance().grpc().lastError()));
    return;
  }

  nlohmann::json j;
  j["total_users"] = total_users;
  j["request_count"] = request_count;
  callback(MakeJsonResponse(drogon::k200OK, j.dump()));
}

void UserController::health(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  nlohmann::json j;
  j["status"] = "ok";
  j["instance"] = GatewayContext::instance().instanceId();
  callback(MakeJsonResponse(drogon::k200OK, j.dump()));
}

}  // namespace http_server_demo
