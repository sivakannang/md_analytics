#pragma once

// Serves the built website (`npm run build` → web/dist) from mda-gateway, so pages, REST and
// the live stream share one origin without the Vite dev server (ADR-0027, TS1).
//
// - Files only from inside the root (no "..", no symlinks out, no directory listings).
// - Hashed build assets (/assets/…) are cached for a year; index.html is revalidated.
// - Any other path without a file extension is an app route: index.html (client routing).
// - Security headers on every response; a strict Content-Security-Policy on pages.

#include <filesystem>

#include <mda/gateway/http.hpp>

namespace mda::gateway {

class StaticSite {
public:
    explicit StaticSite(std::filesystem::path root);

    /// A GET or HEAD for a non-/api path: the file, index.html for an app route, or 404.
    [[nodiscard]] Response serve(const Request& request) const;

    /// The root holds an index.html (a build exists).
    [[nodiscard]] bool available() const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;  ///< canonical
};

}  // namespace mda::gateway
