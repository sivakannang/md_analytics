// mda-useradm — manage user roles from the command line (ADR-0019).
//
//   mda-useradm [--config config/mda.yaml] list
//   mda-useradm [--config config/mda.yaml] grant-admin <username>
//   mda-useradm [--config config/mda.yaml] revoke-admin <username>
//
// Roles can only be changed here, by someone with access to this machine and the app DB,
// never through the web API: a stolen browser session cannot make itself admin.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <mda/admin/admin.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/runtime/config.hpp>
#include <mda/storage/database.hpp>

namespace {

using namespace mda;

int usage() {
    std::fprintf(stderr,
                 "usage: mda-useradm [--config FILE] list\n"
                 "       mda-useradm [--config FILE] grant-admin <username>\n"
                 "       mda-useradm [--config FILE] revoke-admin <username>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config") {
            ++i;  // value consumed by config_path_from_args
            continue;
        }
        args.push_back(arg);
    }
    if (args.empty()) {
        return usage();
    }

    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    auto db = storage::Database::open(config->database.path);
    if (!db) {
        std::fprintf(stderr, "error: cannot open %s: %s\n", config->database.path.c_str(),
                     db.error().message.c_str());
        return 1;
    }
    if (auto migrated = db->migrate(appdb::migrations()); !migrated) {
        std::fprintf(stderr, "error: migrating: %s\n", migrated.error().message.c_str());
        return 1;
    }

    const std::string_view command = args[0];
    if (command == "list" && args.size() == 1) {
        const std::int64_t now = auth::system_clock_seconds();
        const auto users = admin::list_users(*db, admin::ist_day(now), now - 30 * 86'400);
        if (!users) {
            std::fprintf(stderr, "error: %s\n", users.error().message.c_str());
            return 1;
        }
        std::printf("%-6s %-24s %-6s %s\n", "ID", "USERNAME", "ROLE", "ACTIVE SESSIONS");
        for (const auto& user : *users) {
            std::printf("%-6lld %-24s %-6s %lld\n", static_cast<long long>(user.id),
                        user.username.c_str(), user.role.c_str(),
                        static_cast<long long>(user.active_sessions));
        }
        return 0;
    }
    if ((command == "grant-admin" || command == "revoke-admin") && args.size() == 2) {
        const std::string username = auth::AuthService::normalize_username(args[1]);
        const std::string_view role = command == "grant-admin" ? "admin" : "user";
        const auto changed = admin::set_role(*db, username, role);
        if (!changed) {
            std::fprintf(stderr, "error: %s\n", changed.error().message.c_str());
            return 1;
        }
        if (!*changed) {
            std::fprintf(stderr, "error: no user '%.*s'\n", static_cast<int>(args[1].size()),
                         args[1].data());
            return 1;
        }
        std::printf("%s is now %.*s (takes effect on their next request)\n", username.c_str(),
                    static_cast<int>(role.size()), role.data());
        return 0;
    }
    return usage();
}
