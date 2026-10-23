// The built website served by the gateway (ADR-0027, TS1): files from inside the root only,
// app routes to index.html, caching and security headers, and the router's fallback.

#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <unistd.h>

#include <mda/gateway/http.hpp>
#include <mda/gateway/static_site.hpp>

namespace mda::gateway {
namespace {

namespace fs = std::filesystem;

Request get(std::string target, http::verb verb = http::verb::get) {
    Request request{verb, target, 11};
    return request;
}

class StaticSiteTest : public ::testing::Test {
protected:
    StaticSiteTest() {
        base_ = fs::temp_directory_path() / ("mda_site_test_" + std::to_string(getpid()));
        fs::create_directories(base_ / "dist" / "assets");
        fs::create_directories(base_ / "outside");
        std::ofstream(base_ / "dist" / "index.html") << "<!doctype html><title>app</title>";
        std::ofstream(base_ / "dist" / "assets" / "index-abc123.js") << "console.log(1)";
        std::ofstream(base_ / "dist" / "favicon.svg") << "<svg/>";
        std::ofstream(base_ / "outside" / "secret.txt") << "secret";
        fs::create_directory_symlink(base_ / "outside", base_ / "dist" / "leak");
    }
    ~StaticSiteTest() override { fs::remove_all(base_); }

    fs::path base_;
};

TEST_F(StaticSiteTest, ServesPagesAndAssets) {
    const StaticSite site(base_ / "dist");
    ASSERT_TRUE(site.available());

    const Response index = site.serve(get("/"));
    EXPECT_EQ(index.result(), http::status::ok);
    EXPECT_EQ(index.body(), "<!doctype html><title>app</title>");
    EXPECT_EQ(index[http::field::content_type], "text/html; charset=utf-8");
    EXPECT_EQ(index[http::field::cache_control], "no-cache");
    EXPECT_NE(index[http::field::content_security_policy].find("default-src 'self'"),
              std::string::npos);
    EXPECT_EQ(index["X-Frame-Options"], "DENY");
    EXPECT_EQ(index[http::field::x_content_type_options], "nosniff");

    // App routes (client-side routing) get the page.
    EXPECT_EQ(site.serve(get("/chain?u=NIFTY")).body(), index.body());
    EXPECT_EQ(site.serve(get("/admin/system")).body(), index.body());

    const Response script = site.serve(get("/assets/index-abc123.js"));
    EXPECT_EQ(script.result(), http::status::ok);
    EXPECT_EQ(script.body(), "console.log(1)");
    EXPECT_EQ(script[http::field::content_type], "text/javascript; charset=utf-8");
    EXPECT_EQ(script[http::field::cache_control], "public, max-age=31536000, immutable");
    EXPECT_EQ(site.serve(get("/favicon.svg"))[http::field::content_type], "image/svg+xml");

    // A missing asset is a 404, not the page.
    EXPECT_EQ(site.serve(get("/assets/gone-999.js")).result(), http::status::not_found);
}

TEST_F(StaticSiteTest, NeverServesOutsideTheRoot) {
    const StaticSite site(base_ / "dist");
    for (const char* target : {"/../outside/secret.txt", "/assets/../../outside/secret.txt",
                               "/%2e%2e/outside/secret.txt", "//etc/passwd", "/leak/secret.txt",
                               "/assets\\..\\..\\outside\\secret.txt"}) {
        const Response response = site.serve(get(target));
        EXPECT_EQ(response.result(), http::status::not_found) << target;
        EXPECT_EQ(response.body().find("secret"), std::string::npos) << target;
    }
    // A path through the symlink without an extension is an app route: the page, not the folder.
    EXPECT_EQ(site.serve(get("/leak")).body(), "<!doctype html><title>app</title>");
}

TEST_F(StaticSiteTest, AnswersHeadRefusesOtherMethodsAndExplainsAMissingBuild) {
    const StaticSite site(base_ / "dist");
    const Response head = site.serve(get("/assets/index-abc123.js", http::verb::head));
    EXPECT_EQ(head.result(), http::status::ok);
    EXPECT_TRUE(head.body().empty());
    EXPECT_EQ(head[http::field::content_length], "14");
    EXPECT_EQ(site.serve(get("/", http::verb::post)).result(), http::status::method_not_allowed);

    const StaticSite unbuilt(base_ / "nothing-here");
    EXPECT_FALSE(unbuilt.available());
    const Response missing = unbuilt.serve(get("/"));
    EXPECT_EQ(missing.result(), http::status::not_found);
    EXPECT_NE(missing.body().find("npm --prefix web run build"), std::string::npos);
}

TEST(RouterFallbackTest, SendsOnlyNonApiPathsToTheFallback) {
    Router router;
    router.add(http::verb::get, "/api/v1/ping",
               [](const Request&, const PathParams&) -> asio::awaitable<Response> {
                   co_return success("pong");
               });
    router.set_fallback([](const Request&, const PathParams&) -> asio::awaitable<Response> {
        Response response{http::status::ok, 11};
        response.body() = "page";
        co_return response;
    });
    asio::io_context io;
    const auto run = [&](std::string target) {
        const Request request =
            get(std::move(target));  // dispatch keeps a reference: keep it alive
        auto future = asio::co_spawn(io, router.dispatch(request), asio::use_future);
        io.restart();
        io.run();
        return future.get();
    };
    EXPECT_EQ(run("/api/v1/ping").result(), http::status::ok);
    EXPECT_EQ(run("/chain").body(), "page");
    const Response api_missing = run("/api/v1/nope");
    EXPECT_EQ(api_missing.result(), http::status::not_found);
    EXPECT_NE(api_missing.body().find("NotFoundException"),
              std::string::npos);  // JSON, not the page
}

}  // namespace
}  // namespace mda::gateway
