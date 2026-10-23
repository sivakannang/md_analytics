#pragma once

// Runs one coroutine to completion on a private io_context — for tools and startup
// steps that run before the main event loop. Exceptions inside the coroutine (only
// unexpected ones; Kite errors are values) are rethrown.

#include <exception>
#include <optional>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

namespace mda::kite {

template <typename T>
T run_sync(boost::asio::awaitable<T> coroutine) {
    boost::asio::io_context io;
    std::optional<T> result;
    std::exception_ptr failure;
    boost::asio::co_spawn(io, std::move(coroutine), [&](std::exception_ptr e, T value) {
        failure = e;
        if (!e) {
            result.emplace(std::move(value));
        }
    });
    io.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return std::move(*result);
}

}  // namespace mda::kite
