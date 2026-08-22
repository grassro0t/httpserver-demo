#include "common/config.h"

#include <cstdlib>

namespace http_server_demo {

std::map<std::string, std::string> ParseArgs(int argc, char** argv) {
  std::map<std::string, std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto pos = a.find("--");
    if (pos == 0) {
      auto eq = a.find('=');
      if (eq != std::string::npos) {
        args[a.substr(2, eq - 2)] = a.substr(eq + 1);
      } else {
        args[a.substr(2)] = "";
      }
    }
  }
  return args;
}

std::string GetArg(const std::map<std::string, std::string>& args,
                   const std::string& key, const std::string& def) {
  auto it = args.find(key);
  if (it != args.end()) {
    return it->second;
  }
  // 也支持环境变量回退
  const char* env = std::getenv(("HTTP_SERVER_DEMO_" + key).c_str());
  return (env != nullptr) ? env : def;
}

}  // namespace http_server_demo
