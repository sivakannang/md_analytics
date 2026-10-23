# mda::notify — notification delivery: outbox fan-out and retries, templates, channels
# (ADR-0016, docs/design/notifications.md).

find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(boost_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(mda_notify STATIC
    src/codes.cpp
    src/email_channel.cpp
    src/outbox.cpp
    src/secrets.cpp
    src/system_alerts.cpp
    src/telegram.cpp
    src/templates.cpp)
add_library(mda::notify ALIAS mda_notify)

target_include_directories(mda_notify PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_notify PUBLIC mda::storage Boost::asio Boost::beast Boost::json OpenSSL::SSL OpenSSL::Crypto)
mda_target_defaults(mda_notify)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
