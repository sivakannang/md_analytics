#pragma once

// HTTP/1.1 server on Boost.Beast with one coroutine per connection (ADR-0013).
// Limits: 8 KB headers, 64 KB body, 15 s per request; keep-alive supported.
// WebSocket upgrades for one path can be handed to an UpgradeHandler (the live stream).

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include <mda/gateway/http.hpp>

namespace mda::gateway {

/// Takes over a connection whose request asked for a WebSocket upgrade. It owns the
/// stream from then on (accepting or refusing the upgrade).
using UpgradeHandler =
    std::function<asio::awaitable<void>(boost::beast::tcp_stream stream, Request request)>;

/// Called after each HTTP response is written, with the bytes written (header + body).
using ResponseObserver = std::function<void(const Request& request, std::size_t bytes)>;

class HttpServer {
public:
    HttpServer(asio::any_io_executor executor, const Router& router);

    /// Binds and starts accepting. Port 0 picks a free port (tests).
    [[nodiscard]] boost::system::error_code listen(const asio::ip::tcp::endpoint& endpoint);
    [[nodiscard]] asio::ip::tcp::endpoint local_endpoint() const;
    /// Routes WebSocket upgrade requests for `path` (query ignored) to `handler`.
    void on_upgrade(std::string path, UpgradeHandler handler);
    /// Observes every written response (the gateway meters per-user traffic with it).
    void on_response(ResponseObserver observer) { observer_ = std::move(observer); }
    /// Stops accepting new connections (in-flight requests finish).
    void stop();

private:
    asio::awaitable<void> accept_loop();
    asio::awaitable<void> serve(asio::ip::tcp::socket socket);

    asio::any_io_executor executor_;
    const Router* router_;
    asio::ip::tcp::acceptor acceptor_;
    std::string upgrade_path_;
    UpgradeHandler upgrade_;
    ResponseObserver observer_;
};

}  // namespace mda::gateway
