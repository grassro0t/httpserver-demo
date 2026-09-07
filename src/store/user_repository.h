#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "store/mysql_pool.h"
#include "store/redis_client.h"
#include "user/v1/user.pb.h"

namespace http_server_demo {

// 用户数据访问层：MySQL 持久化 + Redis 缓存（Cache-Aside 模式）
class UserRepository {
 public:
  UserRepository(std::shared_ptr<MysqlPool> pool, std::shared_ptr<RedisClient> redis);

  bool CreateUser(const std::string& name, const std::string& email, user::v1::User* out);
  std::optional<user::v1::User> GetUser(const std::string& id);
  bool ListUsers(int limit, int offset, std::vector<user::v1::User>* out, int64_t* total);
  bool GetStats(int64_t* total_users, int64_t* request_count);

  // 每次业务请求计数（Redis INCR）
  void CountRequest();

 private:
  void InvalidateListCache();

  std::shared_ptr<MysqlPool> pool_;
  std::shared_ptr<RedisClient> redis_;
};

}  // namespace http_server_demo
