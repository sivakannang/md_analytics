# mda::auth — app authentication: Argon2id passwords, session tokens, user store and
# rules (ADR-0014, docs/design/user-management.md).

find_package(OpenSSL REQUIRED)

add_library(mda_auth STATIC
    src/auth_service.cpp
    src/password.cpp
    src/session_token.cpp
    src/user_store.cpp)
add_library(mda::auth ALIAS mda_auth)

target_include_directories(mda_auth PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_auth PUBLIC mda::storage mda::appdb PRIVATE OpenSSL::Crypto)
mda_target_defaults(mda_auth)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
