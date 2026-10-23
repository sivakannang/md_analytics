# mda::gateway — HTTP API and live stream for the dashboard (ADR-0013, ADR-0018).

find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(boost_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(mda_gateway STATIC
    src/bars_api.cpp
    src/chain_api.cpp
    src/feed_metrics.cpp
    src/bus_feed.cpp
    src/http.cpp
    src/last_quotes.cpp
    src/live_bars.cpp
    src/notifications_api.cpp
    src/oi_change_api.cpp
    src/quote_cache.cpp
    src/server.cpp
    src/static_site.cpp
    src/system_api.cpp
    src/system_monitor.cpp
    src/stream_api.cpp
    src/usage_tracker.cpp
    src/admin_api.cpp
    src/user_api.cpp
    src/watchlist_api.cpp)
add_library(mda::gateway ALIAS mda_gateway)

target_include_directories(mda_gateway PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_gateway PUBLIC mda::admin mda::auth mda::bars mda::bus mda::notify mda::options mda::quotestore mda::sysmon mda::catalog mda::messages mda::watchlists Boost::asio Boost::beast Boost::json
                      PRIVATE spdlog::spdlog)
mda_target_defaults(mda_gateway)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
