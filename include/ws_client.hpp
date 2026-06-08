#pragma once
#include <functional>
#include <memory>
#include <string>
#include "config.hpp"
#include "metrics.hpp"

namespace blob {

// Abstract transport. The live implementation (ws_client_beast.cpp) uses
// Boost.Beast + OpenSSL; the stub (ws_client_stub.cpp) lets the core build and
// the replay path run without network libraries.
class IWsClient {
public:
  // Called for every inbound text frame, on the network thread.
  using OnMessage = std::function<void(const std::string& frame,
                                       uint64_t conn_epoch, uint64_t conn_seq)>;

  virtual ~IWsClient() = default;
  // Blocking run loop: connect, (re)subscribe, deliver frames until stop().
  // Handles ping/pong, the 24h forced disconnect and reconnect with backoff;
  // each reconnect increments conn_epoch via the supplied metrics/callback.
  virtual int run(const Config& cfg, Metrics& m, OnMessage on_msg) = 0;
  virtual void stop() = 0;
};

// Factory: returns a fresh client instance (one per shard / connection).
std::unique_ptr<IWsClient> make_ws_client();

}  // namespace blob
