#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "store/mysql_pool.h"
#include "store/redis_client.h"
#include "user/v1/user.pb.h"

namespace http_server_demo {

// =============================================================================
// 用户数据访问层（UserRepository）
// =============================================================================
// 本层是 grpc-server 业务逻辑(service)与底层存储之间的"唯一出入口"，封装了
// "MySQL 持久化 + Redis 缓存"这套 Cache-Aside 模式的全部细节：
//
//   读路径（Cache-Aside）：先查 Redis 缓存
//       → 命中：直接返回（不碰数据库，快）
//       → 未命中：查 MySQL → 把结果回填进 Redis（设 TTL）→ 返回
//   写路径：先写 MySQL（保证数据落盘，不丢）
//       → 再主动写/失效相关缓存（保证"写后读到的是新数据"）
//
// 为什么业务层不直接用 pool_/redis_？因为"先缓存还是先数据库、命中了怎么
// 处理、写后如何失效"这套决策如果散落在 service 各处，改动一处容易漏另外
// 几处。收拢到本类，service 只关心业务语义（建用户/查用户），存储策略变化
// 也只改这一个文件。
//
// 依赖的两个底层组件（已有详细注释）：
//   MysqlPool    —— 连接池，acquire() 借连接 / Guard 自动归还（线程安全）
//   RedisClient  —— hiredis 封装，提供带 TTL 的缓存命令
class UserRepository {
 public:
  // 通过依赖注入获得连接池与缓存客户端；用 shared_ptr 便于在 service 层共享
  UserRepository(std::shared_ptr<MysqlPool> pool, std::shared_ptr<RedisClient> redis);

  // 创建用户（写路径）：先 INSERT MySQL，再写/失效缓存。成功返回 true，
  // 并把含数据库自增 id 的完整 User 通过 out 带回。
  bool CreateUser(const std::string& name, const std::string& email, user::v1::User* out);

  // 按 id 查单个用户（读路径）：缓存优先，未命中回源 MySQL 并回填。
  // 返回 std::optional：用户不存在 → nullopt（与"查到了但字段为空"区分开）
  std::optional<user::v1::User> GetUser(const std::string& id);

  // 分页列出用户（读路径，带列表缓存）；total 返回符合条件的总条数。
  // 列表结果与"分页参数"一起缓存，因此不同页码各有独立缓存键。
  bool ListUsers(int limit, int offset, std::vector<user::v1::User>* out, int64_t* total);

  // 统计信息：total_users（COUNT(*) 的缓存值）+ request_count（Redis 计数）
  bool GetStats(int64_t* total_users, int64_t* request_count);

  // 请求计数：每次业务 RPC +1（Redis INCR 原子自增，无 TTL 长期累计）
  void CountRequest();

 private:
  // 新增用户后，把"所有列表缓存"全部删掉（因为任何分页都可能已过期）。
  // 用 Redis 的 Set 记录已产生的列表缓存 key（见 ListUsers），这里枚举删除。
  void InvalidateListCache();

  std::shared_ptr<MysqlPool> pool_;     // MySQL 连接池（持久层）
  std::shared_ptr<RedisClient> redis_;  // Redis 缓存客户端（缓存层）
};

}  // namespace http_server_demo
