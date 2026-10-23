#include <cstdlib>
#include <fstream>
#include <sstream>

#include <boost/json.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mda/kite/session.hpp>

namespace mda::kite {
namespace {

std::string string_field(const boost::json::object& object, std::string_view name) {
    const auto* value = object.if_contains(name);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return std::string(value->as_string());
}

/// Reads and parses the file; no field requirements.
std::expected<LoadedSession, SessionError> read_file(const std::filesystem::path& file) {
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return std::unexpected(SessionError::NotFound);
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return std::unexpected(SessionError::Unreadable);
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();

    boost::system::error_code parse_error;
    const boost::json::value json = boost::json::parse(buffer.str(), parse_error);
    if (parse_error || !json.is_object()) {
        return std::unexpected(SessionError::Malformed);
    }
    const auto& object = json.as_object();
    LoadedSession loaded{.session = {.api_key = string_field(object, "api_key"),
                                     .access_token = string_field(object, "access_token"),
                                     .api_secret = string_field(object, "api_secret"),
                                     .user_id = string_field(object, "user_id")},
                         .source = file.string()};
    struct stat info{};
    if (::stat(file.c_str(), &info) == 0) {
        loaded.insecure_permissions = (info.st_mode & (S_IRWXG | S_IRWXO)) != 0;
    }
    return loaded;
}

}  // namespace

std::expected<LoadedSession, SessionError> load_session(const std::filesystem::path& file) {
    const char* env_key = std::getenv("MDA_KITE_API_KEY");
    const char* env_token = std::getenv("MDA_KITE_ACCESS_TOKEN");
    if (env_key != nullptr && env_token != nullptr && *env_key != '\0' && *env_token != '\0') {
        return LoadedSession{.session = {.api_key = env_key, .access_token = env_token},
                             .source = "environment"};
    }
    auto loaded = read_file(file);
    if (loaded && (loaded->session.api_key.empty() || loaded->session.access_token.empty())) {
        return std::unexpected(SessionError::MissingField);
    }
    return loaded;
}

std::expected<LoadedSession, SessionError> load_login_credentials(
    const std::filesystem::path& file) {
    auto loaded = read_file(file);
    if (loaded && (loaded->session.api_key.empty() || loaded->session.api_secret.empty())) {
        return std::unexpected(SessionError::MissingField);
    }
    return loaded;
}

std::expected<void, SessionError> save_session(const std::filesystem::path& file,
                                               const Session& session) {
    boost::json::object object;
    object["api_key"] = session.api_key;
    if (!session.api_secret.empty()) {
        object["api_secret"] = session.api_secret;
    }
    object["access_token"] = session.access_token;
    if (!session.user_id.empty()) {
        object["user_id"] = session.user_id;
    }
    const std::string text = boost::json::serialize(object) + "\n";

    if (file.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    const std::filesystem::path temp = file.string() + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return std::unexpected(SessionError::WriteFailed);
    }
    ::fchmod(fd, S_IRUSR | S_IWUSR);  // 0600 even if the temp file already existed
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    // rename() is atomic: readers see either the old file or the new one, never a mix.
    ok = ok && ::rename(temp.c_str(), file.c_str()) == 0;
    if (!ok) {
        ::unlink(temp.c_str());
        return std::unexpected(SessionError::WriteFailed);
    }
    return {};
}

std::string_view to_string(SessionError error) noexcept {
    switch (error) {
        case SessionError::NotFound:
            return "no session: set MDA_KITE_API_KEY/MDA_KITE_ACCESS_TOKEN or create the session "
                   "file";
        case SessionError::Unreadable:
            return "session file unreadable";
        case SessionError::Malformed:
            return "session file is not a JSON object";
        case SessionError::MissingField:
            return "session file lacks a required field (api_key, access_token, or for "
                   "mda-login api_secret)";
        case SessionError::WriteFailed:
            return "could not write the session file";
    }
    return "unknown session error";
}

std::string redact(std::string_view secret) {
    return std::string(secret.substr(0, 4)) + "…";
}

}  // namespace mda::kite
