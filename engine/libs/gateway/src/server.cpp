#include <chrono>
#include <exception>
#include <optional>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket/rfc6455.hpp>

#include <mda/gateway/server.hpp>

namespace mda::gateway {
namespace {

namespace beast = boost::beast;
using asio::ip::tcp;
constexpr auto kUseTuple = asio::as_tuple(asio::use_awaitable);

constexpr std::size_t kHeaderLimit = 8 * 1024;
constexpr std::size_t kBodyLimit = 64 * 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(15);

/// The first comma-separated value of a header, trimmed ("" if none or implausibly long).
std::string_view first_value(std::string_view value) {
    value = value.substr(0, value.find(','));
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    while (!value.empty() && value.back() == ' ') {
        value.remove_suffix(1);
    }
    return value.size() <= 255 ? value : std::string_view{};
}

/// Stamps the internal headers (ADR-0019, ADR-0027): the client's address, whether it came
/// over HTTPS, and the host it used. The X-Forwarded-* headers are believed only when the
/// peer is this machine (our dev proxy, or `tailscale serve`): a remote client can't fake them.
void stamp(const beast::tcp_stream& stream, Request& request) {
    for (const auto header : {kClientIpHeader, kViaHttpsHeader, kPublicHostHeader}) {
        request.erase(header);  // only the server sets them
    }
    boost::system::error_code ec;
    const auto peer = stream.socket().remote_endpoint(ec);
    if (ec) {
        return;
    }
    std::string address = peer.address().to_string();
    if (peer.address().is_loopback()) {
        if (const auto forwarded = first_value(request[http::field::x_forwarded_for]);
            !forwarded.empty() && forwarded.size() <= 64) {
            address = std::string(forwarded);
        }
        if (first_value(request["X-Forwarded-Proto"]) == "https") {
            request.set(kViaHttpsHeader, "1");
        }
        if (const auto host = first_value(request["X-Forwarded-Host"]); !host.empty()) {
            request.set(kPublicHostHeader, std::string(host));
        }
    }
    request.set(kClientIpHeader, address);
}

}  // namespace

HttpServer::HttpServer(asio::any_io_executor executor, const Router& router)
    : executor_(executor), router_(&router), acceptor_(executor) {}

boost::system::error_code HttpServer::listen(const tcp::endpoint& endpoint) {
    boost::system::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (!ec) {
        acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    }
    if (!ec) {
        acceptor_.bind(endpoint, ec);
    }
    if (!ec) {
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    }
    if (!ec) {
        asio::co_spawn(executor_, accept_loop(), asio::detached);
    }
    return ec;
}

tcp::endpoint HttpServer::local_endpoint() const {
    return acceptor_.local_endpoint();
}

void HttpServer::on_upgrade(std::string path, UpgradeHandler handler) {
    upgrade_path_ = std::move(path);
    upgrade_ = std::move(handler);
}

void HttpServer::stop() {
    boost::system::error_code ignored;
    acceptor_.close(ignored);
}

asio::awaitable<void> HttpServer::accept_loop() {
    while (acceptor_.is_open()) {
        auto [error, socket] = co_await acceptor_.async_accept(kUseTuple);
        if (error) {
            if (error == asio::error::operation_aborted) {
                co_return;  // stop()
            }
            continue;  // e.g. too many open files: keep serving others
        }
        asio::co_spawn(executor_, serve(std::move(socket)), asio::detached);
    }
}

asio::awaitable<void> HttpServer::serve(tcp::socket socket) {
    beast::tcp_stream stream(std::move(socket));
    beast::flat_buffer buffer;
    while (true) {
        stream.expires_after(kRequestTimeout);
        http::request_parser<http::string_body> parser;
        parser.header_limit(kHeaderLimit);
        parser.body_limit(kBodyLimit);
        auto [read_error, read_bytes] =
            co_await http::async_read(stream, buffer, parser, kUseTuple);
        (void)read_bytes;

        Response response;
        bool keep_alive = false;
        std::optional<Request> observed;  // for the response observer
        if (read_error == http::error::body_limit || read_error == http::error::header_limit) {
            response =
                failure(http::status::payload_too_large, "InputException", "request too large");
        } else if (read_error) {
            break;  // closed, timed out, or not HTTP
        } else {
            Request request = parser.release();
            stamp(stream, request);
            const std::string_view target = request.target();
            if (upgrade_ && beast::websocket::is_upgrade(request) &&
                target.substr(0, target.find('?')) == upgrade_path_) {
                stream.expires_never();  // the handler sets its own timeouts
                co_await upgrade_(std::move(stream), std::move(request));
                co_return;
            }
            keep_alive = request.keep_alive();
            try {
                response = co_await router_->dispatch(request);
            } catch (const std::exception&) {
                response = failure(http::status::internal_server_error, "GeneralException",
                                   "internal error");
            }
            if (observer_) {
                observed = std::move(request);
            }
        }
        response.version(11);
        response.keep_alive(keep_alive);
        response.prepare_payload();
        auto [write_error, write_bytes] = co_await http::async_write(stream, response, kUseTuple);
        if (observer_ && observed) {
            observer_(*observed, write_bytes);
        }
        if (write_error || !keep_alive) {
            break;
        }
    }
    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
}

}  // namespace mda::gateway
