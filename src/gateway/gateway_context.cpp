#include "gateway/gateway_context.h"

namespace http_server_demo {

GatewayContext& GatewayContext::instance() {
  static GatewayContext ctx;
  return ctx;
}

}  // namespace http_server_demo
