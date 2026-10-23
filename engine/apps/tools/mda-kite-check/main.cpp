// mda-kite-check — validate today's Kite session and import the instrument list.
//
//   mda-kite-check [--session PATH] [--ca-file PATH] [--registry PATH] [--save-csv PATH]
//
// Steps: load session -> GET /user/profile (validates the token) -> GET /instruments ->
// import into the instrument registry -> append new ids to the registry file.
// Exit code 0 on success, 2 when the token is expired/invalid, 1 on other failures.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <mda/instruments/registry.hpp>
#include <mda/kite/instrument_import.hpp>
#include <mda/kite/rest_client.hpp>
#include <mda/kite/run_sync.hpp>
#include <mda/kite/session.hpp>

namespace {

using namespace mda;

struct Options {
    std::filesystem::path session = "secrets/kite_session.json";
    std::optional<std::filesystem::path> ca_file;
    std::filesystem::path registry = "data/instruments/registry.csv";
    std::optional<std::filesystem::path> save_csv;
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const char* value = argv[++i];
        if (arg == "--session") {
            options.session = value;
        } else if (arg == "--ca-file") {
            options.ca_file = value;
        } else if (arg == "--registry") {
            options.registry = value;
        } else if (arg == "--save-csv") {
            options.save_csv = value;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

int fail(const kite::KiteError& error) {
    std::fprintf(stderr, "error: %s\n", kite::to_string(error).c_str());
    if (error.code == kite::KiteError::Code::AuthExpired) {
        std::fprintf(stderr,
                     "hint: the access token expires daily (~06:00 IST); log in again "
                     "and update the session file\n");
        return 2;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // keep stdout and stderr lines in order
    const auto options = parse_options(argc, argv);
    if (!options) {
        std::fprintf(stderr,
                     "usage: mda-kite-check [--session PATH] [--ca-file PATH] [--registry PATH] "
                     "[--save-csv PATH]\n");
        return 1;
    }

    // 1. Session
    const auto session = kite::load_session(options->session);
    if (!session) {
        std::fprintf(stderr, "error: %s\n", std::string(kite::to_string(session.error())).c_str());
        return 1;
    }
    std::printf("session    %s  (api_key %s, token %s)\n", session->source.c_str(),
                kite::redact(session->session.api_key).c_str(),
                kite::redact(session->session.access_token).c_str());
    if (session->insecure_permissions) {
        std::fprintf(stderr, "warning: %s is readable by others; run: chmod 600 %s\n",
                     options->session.c_str(), options->session.c_str());
    }

    // 2. TLS
    const auto ca_file = options->ca_file ? options->ca_file : kite::default_ca_file();
    if (!ca_file) {
        std::fprintf(stderr, "error: no CA bundle found; pass --ca-file\n");
        return 1;
    }
    auto tls = kite::make_tls_context(*ca_file);
    if (!tls) {
        return fail(tls.error());
    }
    kite::RestClient client(*tls, session->session);

    // 3. Validate the token
    auto start = std::chrono::steady_clock::now();
    const auto profile = kite::run_sync(client.profile());
    if (!profile) {
        return fail(profile.error());
    }
    std::printf("profile    user %s  (%.2f s)\n", profile->user_id.c_str(), seconds_since(start));

    // 4. Download instruments
    start = std::chrono::steady_clock::now();
    const auto csv = kite::run_sync(client.instruments());
    if (!csv) {
        return fail(csv.error());
    }
    std::printf("instruments %.1f MB downloaded  (%.2f s)\n",
                static_cast<double>(csv->size()) / (1024.0 * 1024.0), seconds_since(start));
    if (options->save_csv) {
        std::ofstream(*options->save_csv, std::ios::binary) << *csv;
        std::printf("           saved to %s\n", options->save_csv->c_str());
    }

    // 5. Import into the registry and persist new ids
    auto loaded = instruments::InstrumentRegistry::load(options->registry);
    if (!loaded) {
        std::fprintf(stderr, "error: cannot load registry %s\n", options->registry.c_str());
        return 1;
    }
    instruments::InstrumentRegistry& registry = loaded->registry;
    start = std::chrono::steady_clock::now();
    const auto imported = kite::import_instruments(*csv, registry);
    if (!imported) {
        std::fprintf(stderr, "error: instrument CSV has no header or missing columns\n");
        return 1;
    }
    const auto& stats = imported->stats;
    std::printf(
        "import     %zu rows: %zu imported, %zu new, %zu unsupported, %zu malformed, "
        "%zu unlinked  (%.2f s)\n",
        stats.rows, stats.imported, stats.new_instruments, stats.skipped_unsupported,
        stats.malformed, stats.unlinked, seconds_since(start));

    const auto saved = registry.save_new(options->registry);
    if (!saved) {
        std::fprintf(stderr, "error: cannot write registry %s\n", options->registry.c_str());
        return 1;
    }
    std::printf("registry   %zu instruments, %zu new ids saved to %s\n", registry.size(), *saved,
                options->registry.c_str());

    // 6. A quick sanity look at NIFTY
    const instruments::ContractKey nifty{.exchange = instruments::Exchange::Nse,
                                         .market = instruments::Market::Cash,
                                         .kind = instruments::Kind::Index,
                                         .underlying = "NIFTY 50"};
    const core::InstrumentId nifty_id = registry.find(nifty);
    std::size_t nifty_options = 0;
    std::size_t nifty_futures = 0;
    for (const auto& instrument : registry.all()) {
        if (instrument.listed_today && instrument.underlying == nifty_id && nifty_id.valid()) {
            (instrument.key.kind == instruments::Kind::Option ? nifty_options : nifty_futures)++;
        }
    }
    std::printf("NIFTY 50   id %u: %zu futures, %zu options listed today\n", nifty_id.value(),
                nifty_futures, nifty_options);
    return 0;
}
