// mda-login — the daily Kite login (docs/design/kite-connectivity.md §2, §9).
//
//   mda-login [--session PATH] [--redirect-url URL] [--ca-file PATH] [--no-browser]
//             [--timeout SECONDS]
//
// 1. Reads api_key and api_secret from the session file.
// 2. Listens on the app's redirect URL (default http://127.0.0.1:8080/auth/zerodha/callback,
//    loopback only) and opens Kite's login page in the browser.
// 3. After you log in, Kite redirects to the listener with a one-time request_token.
// 4. Exchanges it for today's access_token (POST /session/token), verifies it with
//    GET /user/profile, and atomically rewrites the session file (mode 0600).
//
// Exit code: 0 success, 2 login rejected/expired, 1 any other failure.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <mda/kite/login.hpp>
#include <mda/kite/rest_client.hpp>
#include <mda/kite/run_sync.hpp>
#include <mda/kite/session.hpp>

namespace {

using namespace mda;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;

constexpr std::string_view kDefaultRedirect = "http://127.0.0.1:8080/auth/zerodha/callback";

struct Options {
    std::filesystem::path session = "secrets/kite_session.json";
    std::string redirect_url{kDefaultRedirect};
    std::optional<std::filesystem::path> ca_file;
    bool open_browser = true;
    std::chrono::seconds timeout{300};
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--no-browser") {
            options.open_browser = false;
            continue;
        }
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const char* value = argv[++i];
        if (arg == "--session") {
            options.session = value;
        } else if (arg == "--redirect-url") {
            options.redirect_url = value;
        } else if (arg == "--ca-file") {
            options.ca_file = value;
        } else if (arg == "--timeout") {
            options.timeout = std::chrono::seconds(std::atoi(value));
        } else {
            return std::nullopt;
        }
    }
    return options;
}

std::string page(std::string_view title, std::string_view message) {
    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>md_analytics</title>"
           "<style>body{font-family:system-ui;margin:4rem auto;max-width:32rem}</style></head>"
           "<body><h2>" +
           std::string(title) + "</h2><p>" + std::string(message) + "</p></body></html>";
}

/// Serves requests on the redirect address until a valid callback for our `state`
/// arrives. Other requests (favicon, stray hits, wrong state) get 404/400 and are ignored.
asio::awaitable<kite::Callback> wait_for_callback(tcp::acceptor& acceptor, std::string path,
                                                  std::string state) {
    while (true) {
        auto [accept_error, socket] =
            co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
        if (accept_error) {
            co_return kite::Callback{};  // acceptor closed (timeout)
        }
        beast::tcp_stream stream(std::move(socket));
        stream.expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        auto [read_error, read_bytes] =
            co_await http::async_read(stream, buffer, request, asio::as_tuple(asio::use_awaitable));
        (void)read_bytes;
        if (read_error) {
            continue;
        }

        const kite::Callback callback = kite::parse_callback(request.target());
        http::response<http::string_body> response{http::status::ok, request.version()};
        response.set(http::field::content_type, "text/html; charset=utf-8");
        bool done = false;
        if (callback.path != path) {
            response.result(http::status::not_found);
            response.body() = page("Not found", "");
        } else if (callback.state != state) {
            response.result(http::status::bad_request);
            response.body() = page("Login not recognised",
                                   "This callback is not from the current mda-login run.");
        } else if (callback.status != "success" || callback.request_token.empty()) {
            response.result(http::status::bad_request);
            response.body() = page("Login failed", "Kite did not report a successful login.");
            done = true;
        } else {
            response.body() = page("Logged in",
                                   "md_analytics received the login. You can close "
                                   "this tab and return to the terminal.");
            done = true;
        }
        response.prepare_payload();
        co_await http::async_write(stream, response, asio::as_tuple(asio::use_awaitable));
        if (done) {
            co_return callback;
        }
    }
}

int fail(const kite::KiteError& error) {
    std::fprintf(stderr, "error: %s\n", kite::to_string(error).c_str());
    return error.code == kite::KiteError::Code::AuthExpired ? 2 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const auto options = parse_options(argc, argv);
    if (!options) {
        std::fprintf(stderr,
                     "usage: mda-login [--session PATH] [--redirect-url URL] [--ca-file PATH] "
                     "[--no-browser] [--timeout SECONDS]\n");
        return 1;
    }

    // 1. Credentials
    auto credentials = kite::load_login_credentials(options->session);
    if (!credentials) {
        std::fprintf(stderr, "error: %s (%s)\n",
                     std::string(kite::to_string(credentials.error())).c_str(),
                     options->session.c_str());
        return 1;
    }
    kite::Session session = credentials->session;
    std::printf("credentials %s  (api_key %s, api_secret %s)\n", credentials->source.c_str(),
                kite::redact(session.api_key).c_str(), kite::redact(session.api_secret).c_str());

    const auto redirect = kite::parse_redirect_url(options->redirect_url);
    if (!redirect) {
        std::fprintf(stderr, "error: redirect URL must be http://127.0.0.1:PORT/PATH\n");
        return 1;
    }
    const auto ca_file = options->ca_file ? options->ca_file : kite::default_ca_file();
    if (!ca_file) {
        std::fprintf(stderr, "error: no CA bundle found; pass --ca-file\n");
        return 1;
    }
    auto tls = kite::make_tls_context(*ca_file);
    if (!tls) {
        return fail(tls.error());
    }

    // 2. Listen on the redirect address (loopback only)
    asio::io_context io;
    tcp::acceptor acceptor(io);
    boost::system::error_code ec;
    const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), redirect->port);
    acceptor.open(endpoint.protocol(), ec);
    if (!ec) {
        acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    }
    if (!ec) {
        acceptor.bind(endpoint, ec);
    }
    if (!ec) {
        acceptor.listen(asio::socket_base::max_listen_connections, ec);
    }
    if (ec) {
        std::fprintf(stderr, "error: cannot listen on 127.0.0.1:%u: %s%s\n", redirect->port,
                     ec.message().c_str(),
                     ec == asio::error::address_in_use
                         ? " (another app — e.g. your old login app — is using this port)"
                         : "");
        return 1;
    }

    // 3. Open the login page
    const std::string state = kite::random_state();
    if (state.empty()) {
        std::fprintf(stderr, "error: no secure random source\n");
        return 1;
    }
    const std::string url = kite::login_url(session.api_key, state);
    std::printf("listening   %s\n", options->redirect_url.c_str());
    std::printf("log in at   %s\n", url.c_str());
    if (options->open_browser) {
#ifdef __APPLE__
        const std::string command = "open '" + url + "'";
#else
        const std::string command = "xdg-open '" + url + "' >/dev/null 2>&1";
#endif
        if (std::system(command.c_str()) != 0) {
            std::printf("            (could not open a browser; open the URL above yourself)\n");
        }
    }
    std::printf("waiting     up to %lld s for the login to complete…\n",
                static_cast<long long>(options->timeout.count()));

    // 4. Wait for the callback (or time out)
    std::optional<kite::Callback> callback;
    asio::co_spawn(io, wait_for_callback(acceptor, redirect->path, state),
                   [&](std::exception_ptr failure, kite::Callback received) {
                       if (!failure) {
                           callback = std::move(received);
                       }
                   });
    io.run_for(options->timeout);
    acceptor.close();
    if (!callback || callback->request_token.empty()) {
        std::fprintf(stderr, "error: no successful login received within %lld s\n",
                     static_cast<long long>(options->timeout.count()));
        return callback ? 2 : 1;
    }
    std::printf("callback    request_token %s received\n",
                kite::redact(callback->request_token).c_str());

    // 5. Exchange for today's access token and verify it
    kite::RestClient login_client(*tls, session);
    const auto token = kite::run_sync(login_client.create_session(callback->request_token));
    if (!token) {
        return fail(token.error());
    }
    session.access_token = token->access_token;
    session.user_id = token->user_id;

    kite::RestClient client(*tls, session);
    const auto profile = kite::run_sync(client.profile());
    if (!profile) {
        return fail(profile.error());
    }

    // 6. Save atomically
    if (const auto saved = kite::save_session(options->session, session); !saved) {
        std::fprintf(stderr, "error: %s\n", std::string(kite::to_string(saved.error())).c_str());
        return 1;
    }
    std::printf("logged in   user %s; access token %s saved to %s\n", profile->user_id.c_str(),
                kite::redact(session.access_token).c_str(), options->session.c_str());
    return 0;
}
