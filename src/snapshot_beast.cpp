// REST depth-snapshot fetch over HTTPS (Boost.Beast). Built with ENABLE_LIVE.
// Uses ASYNC operations on an io_context so beast::tcp_stream::expires_after
// actually enforces a deadline (Beast timeouts do NOT apply to synchronous
// calls). A stalled request therefore cannot block the shard or shutdown.
#include "snapshot.hpp"

#if defined(ENABLE_LIVE)
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <openssl/ssl.h>
#include <chrono>
#include <string>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = boost::asio::ssl;
using tcp = net::ip::tcp;

namespace blob {

bool fetch_depth_snapshot(Venue venue, const std::string& symbol, int limit,
                          DepthEvent& out, std::string& err, std::string* raw_json) {
  try {
    const std::string host = (venue == Venue::Spot) ? "api.binance.com" : "fapi.binance.com";
    const std::string path = (venue == Venue::Spot)
        ? "/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(limit)
        : "/fapi/v1/depth?symbol=" + symbol + "&limit=" + std::to_string(limit);
    constexpr auto kTimeout = std::chrono::seconds(10);

    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_verify_mode(ssl::verify_peer);
    ctx.set_default_verify_paths();
    ctx.set_verify_callback(ssl::host_name_verification(host));

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
      err = "SNI failed"; return false;
    }

    beast::flat_buffer buffer;
    http::request<http::empty_body> req{http::verb::get, path, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "binance-lob-capture/1.0");
    http::response<http::string_body> res;

    beast::error_code final_ec;
    bool completed = false;
    auto& low = beast::get_lowest_layer(stream);

    // Async chain: resolve -> connect -> TLS handshake -> write -> read.
    // expires_after() (re-armed before each step) enforces the deadline because
    // these are asynchronous operations.
    resolver.async_resolve(host, "443",
      [&](beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) { final_ec = ec; return; }
        low.expires_after(kTimeout);
        low.async_connect(results,
          [&](beast::error_code ec2, const tcp::endpoint&) {
            if (ec2) { final_ec = ec2; return; }
            low.expires_after(kTimeout);
            stream.async_handshake(ssl::stream_base::client,
              [&](beast::error_code ec3) {
                if (ec3) { final_ec = ec3; return; }
                low.expires_after(kTimeout);
                http::async_write(stream, req,
                  [&](beast::error_code ec4, std::size_t) {
                    if (ec4) { final_ec = ec4; return; }
                    low.expires_after(kTimeout);
                    http::async_read(stream, buffer, res,
                      [&](beast::error_code ec5, std::size_t) {
                        final_ec = ec5; completed = true;
                      });
                  });
              });
          });
      });

    ioc.run();  // returns when the chain finishes or a deadline cancels it

    if (final_ec) { err = final_ec.message(); return false; }
    if (!completed) { err = "incomplete"; return false; }

    beast::error_code ig;
    low.expires_never();
    stream.shutdown(ig);  // ignore truncation on close

    if (res.result() != http::status::ok) {
      err = "HTTP " + std::to_string((int)res.result_int());
      return false;
    }
    out = DepthEvent{};
    if (raw_json) *raw_json = res.body();
    return parse_depth(res.body(), out);
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

}  // namespace blob
#endif  // ENABLE_LIVE
