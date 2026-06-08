// Live Binance WebSocket client (Boost.Beast + OpenSSL). Built with ENABLE_LIVE.
//
// Fully asynchronous on a single io_context: resolve -> connect -> TLS -> WS
// handshake -> read loop are all async, and a 200ms watchdog timer enforces
// stop()/deadline by cancelling the resolver and closing the transport. This
// makes setup, the read loop, and shutdown all promptly cancellable (Beast
// timeouts do NOT apply to synchronous calls, so blocking calls are avoided).
// Reconnect backoff sleeps in short, interruptible steps so SIGINT is honored
// within ~20ms instead of waiting out the full backoff.
#include "ws_client.hpp"

#if defined(ENABLE_LIVE)
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace http = beast::http;
using tcp = net::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace blob {

class WsClientBeast : public IWsClient {
public:
  int run(const Config& cfg, Metrics& m, OnMessage on_msg) override {
    const std::string host = (cfg.venue == Venue::Spot)
        ? "stream.binance.com" : "fstream.binance.com";
    const std::string port = (cfg.venue == Venue::Spot) ? "9443" : "443";
    const std::string target = (cfg.venue == Venue::Spot)
        ? "/stream?streams=" + cfg.combined_streams()
        : "/public/stream?streams=" + cfg.combined_streams();

    uint64_t conn_epoch = 0;
    uint64_t conn_seq = 0;
    int backoff_ms = 1000;
    auto conn_start = Clock::now();
    const auto deadline = Clock::now() +
        std::chrono::seconds(cfg.duration_s > 0 ? cfg.duration_s : 3600 * 24);

    // Interruptible sleep: wakes promptly on stop()/deadline.
    auto interruptible_sleep = [&](int ms) {
      auto until = Clock::now() + std::chrono::milliseconds(ms);
      while (!stop_.load() && Clock::now() < until && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    };

    while (!stop_.load() && Clock::now() < deadline) {
      beast::error_code run_ec;
      try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_default_verify_paths();
        ctx.set_verify_callback(ssl::host_name_verification(host));

        tcp::resolver resolver(ioc);
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ctx);
        auto& low = beast::get_lowest_layer(ws);
        beast::flat_buffer buffer;

        // Watchdog: enforce stop()/deadline by cancelling I/O so ioc.run() ends.
        net::steady_timer wd(ioc);
        std::function<void()> arm = [&]() {
          wd.expires_after(std::chrono::milliseconds(200));
          wd.async_wait([&](const beast::error_code& tec) {
            if (tec) return;
            if (stop_.load() || Clock::now() >= deadline) {
              resolver.cancel();
              ioc.stop();   // force ioc.run() to return now (also drops the
                            // websocket's internal keep-alive/idle timer, which
                            // low.close() alone would leave pending)
              return;
            }
            arm();
          });
        };

        std::function<void()> do_read = [&]() {
          ws.async_read(buffer, [&](beast::error_code ec, std::size_t) {
            if (ec) { run_ec = ec; wd.cancel(); return; }
            std::string frame = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            ++conn_seq;
            try {
              on_msg(frame, conn_epoch, conn_seq);
            } catch (const std::exception& he) {
              m.parse_errors++;
              std::fprintf(stderr, "[ws] handler error (continuing): %s\n", he.what());
            }
            if (stop_.load() || Clock::now() >= deadline) { wd.cancel(); ioc.stop(); return; }
            do_read();
          });
        };

        // Async setup chain.
        resolver.async_resolve(host, port,
          [&](beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) { run_ec = ec; wd.cancel(); return; }
            low.expires_after(std::chrono::seconds(15));
            low.async_connect(results, [&](beast::error_code ec2, const tcp::endpoint&) {
              if (ec2) { run_ec = ec2; wd.cancel(); return; }
              if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
                run_ec = beast::error_code((int)::ERR_get_error(), net::error::get_ssl_category());
                wd.cancel(); return;
              }
              low.expires_after(std::chrono::seconds(15));
              ws.next_layer().async_handshake(ssl::stream_base::client,
                [&](beast::error_code ec3) {
                  if (ec3) { run_ec = ec3; wd.cancel(); return; }
                  websocket::stream_base::timeout to{};
                  to.handshake_timeout = std::chrono::seconds(30);
                  to.idle_timeout      = std::chrono::seconds(20);
                  to.keep_alive_pings  = true;
                  ws.set_option(to);
                  ws.set_option(websocket::stream_base::decorator(
                    [](websocket::request_type& req) {
                      req.set(http::field::user_agent, "binance-lob-capture/1.0");
                    }));
                  low.expires_never();  // beast ws timeout governs from here
                  ws.async_handshake(host, target, [&](beast::error_code ec4) {
                    if (ec4) { run_ec = ec4; wd.cancel(); return; }
                    backoff_ms = 1000;
                    conn_seq = 0;
                    conn_start = Clock::now();
                    do_read();
                  });
                });
            });
          });

        arm();
        ioc.run();  // drives everything until cancelled / errored

        low.close();  // immediate transport close, no blocking handshake

        if (stop_.load() || Clock::now() >= deadline) break;

        if (run_ec) {
          double secs = std::chrono::duration<double>(Clock::now() - conn_start).count();
          std::fprintf(stderr, "[ws] disconnect after %.1fs, %llu msgs (%s); reconnecting...\n",
                       secs, (unsigned long long)conn_seq, run_ec.message().c_str());
        }
        m.reconnects++;
        ++conn_epoch;
        interruptible_sleep(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, 15000);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[ws] error: %s; reconnecting...\n", e.what());
        if (stop_.load() || Clock::now() >= deadline) break;
        m.reconnects++;
        ++conn_epoch;
        interruptible_sleep(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, 15000);
      }
    }
    return 0;
  }
  void stop() override { stop_.store(true); }
private:
  std::atomic<bool> stop_{false};
};

std::unique_ptr<IWsClient> make_ws_client() { return std::make_unique<WsClientBeast>(); }

}  // namespace blob
#endif  // ENABLE_LIVE
