# mda::kite — Zerodha Kite adapter: the only module that knows Kite's formats and
# endpoints (docs/design/kite-adapter.md, docs/design/kite-connectivity.md).
# Only mda-feedd and Kite tools depend on it.

find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(boost_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(mda_kite STATIC
    src/instrument_import.cpp
    src/login.cpp
    src/rest_client.cpp
    src/session.cpp
    src/tick_decoder.cpp
    src/ws_client.cpp)
add_library(mda::kite ALIAS mda_kite)

target_include_directories(mda_kite PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_kite
    PUBLIC mda::core mda::instruments mda::messages Boost::asio Boost::beast OpenSSL::SSL
           OpenSSL::Crypto
    PRIVATE Boost::json)
mda_target_defaults(mda_kite)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
