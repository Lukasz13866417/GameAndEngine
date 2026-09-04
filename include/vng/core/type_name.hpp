#pragma once

#include <string_view>

namespace vng::core {

template<class T>
[[nodiscard]] constexpr std::string_view type_name() noexcept
{
#if defined(__clang__)
    constexpr std::string_view function = __PRETTY_FUNCTION__;
    constexpr std::string_view prefix = "T = ";
    const auto begin = function.find(prefix) + prefix.size();
    const auto end = function.find(']', begin);
    return function.substr(begin, end - begin);
#elif defined(__GNUC__)
    constexpr std::string_view function = __PRETTY_FUNCTION__;
    constexpr std::string_view prefix = "with T = ";
    const auto begin = function.find(prefix) + prefix.size();
    const auto end = function.find(';', begin);
    return function.substr(begin, end - begin);
#else
    return "unknown semantic";
#endif
}

} // namespace vng::core
