#pragma once

#include <map>
#include <string>

namespace http_server_demo {

// 解析 "--key=value" 形式的命令行参数
std::map<std::string, std::string> ParseArgs(int argc, char** argv);

// 取参数，缺失时返回默认值
std::string GetArg(const std::map<std::string, std::string>& args, const std::string& key,
                   const std::string& def);

}  // namespace http_server_demo
