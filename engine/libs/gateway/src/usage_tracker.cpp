#include <mda/auth/session_token.hpp>
#include <mda/gateway/usage_tracker.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

constexpr std::size_t kMaxTokens = 10'000;  // bounded memory: sessions are few

std::string key_of(std::string_view token) {
    const auto hash = auth::hash_token(token);
    return {reinterpret_cast<const char*>(hash.data()), hash.size()};
}

}  // namespace

UsageTracker::UsageTracker(Clock clock) : clock_(std::move(clock)) {}

void UsageTracker::remember_token(std::string_view token, std::int64_t user_id) {
    std::string key = key_of(token);
    const std::scoped_lock lock(mutex_);
    if (token_users_.size() >= kMaxTokens && !token_users_.contains(key)) {
        token_users_.clear();  // re-learnt on the next authentication
    }
    token_users_[std::move(key)] = user_id;
}

void UsageTracker::meter_http(const Request& request, std::size_t bytes) {
    const std::string token = detail::token_of(request);
    if (token.empty()) {
        return;  // anonymous (sign-up, sign-in failures): nobody to charge
    }
    const std::string key = key_of(token);
    const std::string day = admin::ist_day(clock_());
    const std::scoped_lock lock(mutex_);
    const auto user = token_users_.find(key);
    if (user == token_users_.end()) {
        return;
    }
    admin::Usage& usage = pending_[{user->second, day}];
    usage.http_bytes += static_cast<std::int64_t>(bytes);
    usage.http_requests += 1;
}

void UsageTracker::meter_ws(std::int64_t user_id, std::size_t bytes) {
    const std::string day = admin::ist_day(clock_());
    const std::scoped_lock lock(mutex_);
    admin::Usage& usage = pending_[{user_id, day}];
    usage.ws_bytes += static_cast<std::int64_t>(bytes);
    usage.ws_messages += 1;
}

std::uint64_t UsageTracker::open_connection(std::int64_t user_id) {
    const std::scoped_lock lock(mutex_);
    const std::uint64_t id = next_connection_++;
    connections_[id] = {.user_id = user_id, .since = clock_()};
    return id;
}

void UsageTracker::set_instruments(std::uint64_t connection, std::size_t instruments) {
    const std::scoped_lock lock(mutex_);
    if (const auto found = connections_.find(connection); found != connections_.end()) {
        found->second.instruments = instruments;
    }
}

void UsageTracker::close_connection(std::uint64_t connection) {
    const std::scoped_lock lock(mutex_);
    connections_.erase(connection);
}

std::vector<UsageTracker::Connection> UsageTracker::connections() const {
    const std::scoped_lock lock(mutex_);
    std::vector<Connection> out;
    out.reserve(connections_.size());
    for (const auto& [id, connection] : connections_) {
        out.push_back(connection);
    }
    return out;
}

admin::Usage UsageTracker::unsaved_today(std::int64_t user_id) const {
    const std::string day = admin::ist_day(clock_());
    const std::scoped_lock lock(mutex_);
    const auto found = pending_.find({user_id, day});
    return found == pending_.end() ? admin::Usage{} : found->second;
}

std::vector<UsageTracker::Pending> UsageTracker::take() {
    std::map<std::pair<std::int64_t, std::string>, admin::Usage> taken;
    {
        const std::scoped_lock lock(mutex_);
        taken.swap(pending_);
    }
    std::vector<Pending> out;
    out.reserve(taken.size());
    for (auto& [key, usage] : taken) {
        out.push_back({.user_id = key.first, .day = key.second, .usage = usage});
    }
    return out;
}

}  // namespace mda::gateway
