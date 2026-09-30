#pragma once
// The rejection check these applications share. They use no test framework, so it reports a failure by throwing.
#include <exception>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace rejection {
// Requires operation to throw an exception of type Expected, or derived from it, whose what() is exactly
// expected, as doctest's CHECK_THROWS_WITH_AS does. Otherwise throws std::runtime_error naming the call site, the
// expected message and what happened instead: another message, another exception type, or no exception.
template <class Expected, class Operation>
void rejects(Operation &&operation, std::string_view expected,
             std::source_location where = std::source_location::current()) {
    static_assert(std::is_base_of_v<std::exception, Expected>, "Rejections throw standard exceptions");
    std::string outcome = "no exception";
    // Expected is matched with dynamic_cast inside one std::exception handler rather than by a handler of its own:
    // in a Release build with AppleClang 17.0.0, the `catch (const Expected &)` handler at one call site did not
    // catch a matching std::out_of_range, which the std::exception handler after it did.
    try {
        std::forward<Operation>(operation)();
    } catch (const std::exception &error) {
        if (dynamic_cast<const Expected *>(&error)) {
            if (expected == error.what())
                return;
            outcome = '"' + std::string(error.what()) + '"';
        } else
            outcome = "an exception of type " + std::string(typeid(error).name()) + ": \"" + error.what() + '"';
    } catch (...) {
        outcome = "an exception that is not a std::exception";
    }
    throw std::runtime_error(std::string(where.file_name()) + ':' + std::to_string(where.line()) + ": expected \"" +
                             std::string(expected) + "\", got " + outcome);
}
} // namespace rejection
