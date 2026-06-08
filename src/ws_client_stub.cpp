#include "ws_client.hpp"
#include <memory>
#include <cstdio>

namespace blob {

// Build-without-network stub. Lets the executable link and the replay path run
// when ENABLE_LIVE is off. Live capture requires -DENABLE_LIVE=ON (Boost+SSL).
class WsClientStub : public IWsClient {
public:
  int run(const Config&, Metrics&, OnMessage) override {
    std::fprintf(stderr,
      "[ws] Live capture not built. Reconfigure with -DENABLE_LIVE=ON "
      "(needs Boost.Beast + OpenSSL), or use --replay <market_data.csv>.\n");
    return 3;
  }
  void stop() override {}
};

std::unique_ptr<IWsClient> make_ws_client() { return std::make_unique<WsClientStub>(); }

}  // namespace blob
