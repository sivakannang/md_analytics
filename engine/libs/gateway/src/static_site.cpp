#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>

#include <mda/gateway/static_site.hpp>

namespace mda::gateway {
namespace {

/// The site loads nothing from elsewhere: scripts, styles, images and the stream are all
/// same-origin. Inline style attributes (React) need 'unsafe-inline' for styles only.
constexpr std::string_view kContentSecurityPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' "
    "data:; "
    "font-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'; "
    "frame-ancestors 'none'";

std::string_view content_type(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".html")
        return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs")
        return "text/javascript; charset=utf-8";
    if (ext == ".css")
        return "text/css; charset=utf-8";
    if (ext == ".json" || ext == ".map")
        return "application/json";
    if (ext == ".webmanifest")
        return "application/manifest+json";
    if (ext == ".svg")
        return "image/svg+xml";
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".ico")
        return "image/x-icon";
    if (ext == ".woff2")
        return "font/woff2";
    if (ext == ".txt")
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool read_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    out = std::move(text).str();
    return true;
}

/// The URL path, or empty when it can't be a file of the site: no "..", backslashes, NULs
/// or percent-escapes (the build's file names never need them).
std::string safe_path(std::string_view target) {
    std::string_view path = target.substr(0, target.find('?'));
    path = path.substr(0, path.find('#'));
    if (path.empty() || path.front() != '/' || path.find("..") != std::string_view::npos ||
        path.find('\\') != std::string_view::npos || path.find('%') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos || path.find("//") != std::string_view::npos) {
        return {};
    }
    return std::string(path);
}

Response plain(http::status status, unsigned version, std::string_view text) {
    Response response{status, version};
    response.set(http::field::content_type, "text/plain; charset=utf-8");
    response.set(http::field::x_content_type_options, "nosniff");
    response.body() = text;
    response.prepare_payload();
    return response;
}

}  // namespace

StaticSite::StaticSite(std::filesystem::path root) {
    std::error_code ec;
    root_ = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        root_ = std::move(root);
    }
}

bool StaticSite::available() const {
    std::error_code ec;
    return std::filesystem::is_regular_file(root_ / "index.html", ec);
}

Response StaticSite::serve(const Request& request) const {
    const unsigned version = request.version();
    if (request.method() != http::verb::get && request.method() != http::verb::head) {
        return plain(http::status::method_not_allowed, version, "method not allowed");
    }
    const std::string path = safe_path(request.target());
    if (path.empty()) {
        return plain(http::status::not_found, version, "not found");
    }

    // A real file under the root (after resolving symlinks), else an app route.
    std::filesystem::path file;
    std::error_code ec;
    if (path != "/") {
        const auto candidate = std::filesystem::weakly_canonical(root_ / path.substr(1), ec);
        const bool inside =
            std::mismatch(root_.begin(), root_.end(), candidate.begin(), candidate.end()).first ==
            root_.end();
        if (!ec && inside && std::filesystem::is_regular_file(candidate, ec)) {
            file = candidate;
        }
    }
    const bool is_index = file.empty();
    if (is_index) {
        // "/chain" is a page of the app; "/assets/gone.js" is a missing file, not a page.
        const std::string_view last = std::string_view(path).substr(path.rfind('/') + 1);
        if (last.find('.') != std::string_view::npos) {
            return plain(http::status::not_found, version, "not found");
        }
        file = root_ / "index.html";
    }

    std::string body;
    if (!read_file(file, body)) {
        return plain(
            http::status::not_found, version,
            is_index ? "the website is not built (npm --prefix web run build)" : "not found");
    }
    Response response{http::status::ok, version};
    response.set(http::field::content_type, content_type(file));
    response.set(http::field::x_content_type_options, "nosniff");
    response.set(http::field::referrer_policy, "same-origin");
    response.set(http::field::cache_control, path.starts_with("/assets/") && !is_index
                                                 ? "public, max-age=31536000, immutable"
                                                 : "no-cache");
    if (file.extension() == ".html") {
        response.set(http::field::content_security_policy, kContentSecurityPolicy);
        response.set("X-Frame-Options", "DENY");
    }
    const std::size_t size = body.size();
    if (request.method() == http::verb::head) {
        body.clear();
    }
    response.body() = std::move(body);
    response.prepare_payload();
    if (request.method() == http::verb::head) {
        response.content_length(size);
    }
    return response;
}

}  // namespace mda::gateway
