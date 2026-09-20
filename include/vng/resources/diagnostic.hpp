#pragma once

#include <concepts>
#include <expected>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace vng::resources {

enum class ErrorCode {
    no_provider,
    provision_failed,
    invalid_argument,
    validation_failed,
    operation_failed,
};

// Retaining the original error keeps backend codes, source maps, source
// locations, and other domain-specific evidence available without importing
// any backend headers. Even a move-only error can be retained and shared.
class DiagnosticCause final {
public:
    DiagnosticCause() = default;

    template<class E>
        requires (!std::same_as<std::remove_cvref_t<E>, DiagnosticCause>)
    explicit DiagnosticCause(E&& error)
        : value_(std::make_shared<const std::remove_cvref_t<E>>(
              std::forward<E>(error))),
          type_(&typeid(std::remove_cvref_t<E>))
    {}

    template<class E>
    [[nodiscard]] const E* get() const noexcept
    {
        return type_ && *type_ == typeid(E)
            ? static_cast<const E*>(value_.get()) : nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(value_);
    }

private:
    std::shared_ptr<const void> value_;
    const std::type_info* type_{};
};

struct Diagnostic final {
    ErrorCode code{ErrorCode::operation_failed};
    std::string message{};
    std::vector<std::string> context{};
    std::string driver_log{};
    std::string generated_source{};
    DiagnosticCause cause{};

    template<class E>
    [[nodiscard]] const E* cause_as() const noexcept
    {
        return cause.get<E>();
    }

    Diagnostic& note(std::string text)
    {
        context.push_back(std::move(text));
        return *this;
    }
};

template<class T>
using Result = std::expected<T, Diagnostic>;

[[nodiscard]] inline Diagnostic to_diagnostic(Diagnostic diagnostic)
{
    return diagnostic;
}

template<class E>
    requires (!std::same_as<std::remove_cvref_t<E>, Diagnostic>)
[[nodiscard]] Diagnostic to_diagnostic(E&& error)
{
    Diagnostic diagnostic;
    diagnostic.code = ErrorCode::provision_failed;
    if constexpr (requires { std::string{error.message}; }) {
        diagnostic.message = error.message;
    } else if constexpr (requires { std::string{error.message()}; }) {
        diagnostic.message = error.message();
    } else if constexpr (requires { std::string{error.what()}; }) {
        diagnostic.message = error.what();
    } else if constexpr (std::constructible_from<std::string, const E&>) {
        diagnostic.message = std::string{error};
    } else {
        diagnostic.message = "Resource provisioning failed.";
    }
    if constexpr (requires { diagnostic.driver_log = error.driver_log; }) {
        diagnostic.driver_log = error.driver_log;
    }
    if constexpr (requires { diagnostic.generated_source = error.generated_source; }) {
        diagnostic.generated_source = error.generated_source;
    }
    if constexpr (requires { diagnostic.context = error.notes; }) {
        diagnostic.context = error.notes;
    }
    diagnostic.cause = DiagnosticCause{std::forward<E>(error)};
    return diagnostic;
}

// Take the expected by value so successes and original errors can both retain
// move-only ownership. Pass std::move(result) when normalizing a named result.
template<class T, class E>
[[nodiscard]] Result<T> into_result(std::expected<T, E> result)
{
    if (!result) {
        return std::unexpected(to_diagnostic(std::move(result.error())));
    }
    if constexpr (std::is_void_v<T>) {
        return {};
    } else {
        return std::move(*result);
    }
}

struct ReloadReport final {
    std::vector<std::string> refreshed;
    std::vector<std::string> retained;
};

} // namespace vng::resources
