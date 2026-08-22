#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "user/v1/user.pb.h"

namespace http_server_demo {

// proto User <-> nlohmann::json 转换，统一走 JSON 字符串存进 Redis

inline nlohmann::json UserToJson(const user::v1::User& u) {
  return nlohmann::json{{"id", u.id()},
                        {"name", u.name()},
                        {"email", u.email()},
                        {"created_at", u.created_at()}};
}

inline std::string UserToJsonString(const user::v1::User& u) {
  return UserToJson(u).dump();
}

inline void JsonToUser(const nlohmann::json& j, user::v1::User* u) {
  u->set_id(j.value("id", ""));
  u->set_name(j.value("name", ""));
  u->set_email(j.value("email", ""));
  u->set_created_at(j.value("created_at", static_cast<int64_t>(0)));
}

}  // namespace http_server_demo
