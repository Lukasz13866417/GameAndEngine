#pragma once

#include <concepts>
#include <expected>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <vng/resources/diagnostic.hpp>

namespace vng::resources {

// The same const protocol covers CPU sources and context-dependent sources.
// Prefer the complete signature; only drop the context when the source does
// not use one. A request must never be silently ignored.
template<class P, class... Args>
    requires requires(const P& source, Args&&... args) {
        source.provide(std::forward<Args>(args)...);
    }
[[nodiscard]] decltype(auto) provide(const P& source, Args&&... args)
{
    return source.provide(std::forward<Args>(args)...);
}

template<class P, class Context>
    requires (!requires(const P& source, Context& context) {
        source.provide(context);
    }) && requires(const P& source) { source.provide(); }
[[nodiscard]] decltype(auto) provide(const P& source, Context&)
{
    return source.provide();
}

template<class P, class Context, class Request>
    requires (!requires(const P& source, Context& context, const Request& request) {
        source.provide(context, request);
    }) && requires(const P& source, const Request& request) { source.provide(request); }
[[nodiscard]] decltype(auto) provide(const P& source, Context&, const Request& request)
{
    return source.provide(request);
}

template<class F>
class CallableProvider final {
public:
    explicit CallableProvider(F function) : function_(std::move(function)) {}

    template<class... Args>
        requires std::invocable<const F&, Args...>
    [[nodiscard]] decltype(auto) provide(Args&&... args) const
    {
        return std::invoke(function_, std::forward<Args>(args)...);
    }

private:
    F function_;
};

template<class F>
[[nodiscard]] auto provider(F&& function)
{
    return CallableProvider<std::decay_t<F>>{std::forward<F>(function)};
}

namespace detail {

template<bool HasContext, class P, class... Args>
    requires (HasContext && requires(const P& source, Args&&... args) {
        resources::provide(source, std::forward<Args>(args)...);
    }) || (!HasContext && requires(const P& source, Args&&... args) {
        source.provide(std::forward<Args>(args)...);
    })
[[nodiscard]] decltype(auto) invoke_provider(const P& source, Args&&... args)
{
    if constexpr (HasContext) {
        return resources::provide(source, std::forward<Args>(args)...);
    } else {
        return source.provide(std::forward<Args>(args)...);
    }
}

template<class R, class T>
struct expected_product : std::false_type {};

template<class U, class E, class T>
struct expected_product<std::expected<U, E>, T>
    : std::bool_constant<
          (std::is_void_v<U> && std::is_void_v<T>)
          || (!std::is_void_v<U> && !std::is_void_v<T>
              && std::is_constructible_v<T, std::add_rvalue_reference_t<U>>)> {};

template<class P, class T, bool HasContext, class... Args>
concept provides = requires(const P& source, Args... args) {
    invoke_provider<HasContext>(source, args...);
    requires expected_product<
        decltype(invoke_provider<HasContext>(source, args...)), T>::value;
};

template<class T, bool HasContext, class... Args>
class ProviderStorage final {
public:
    ProviderStorage() = default;

    template<class P>
        requires (!std::same_as<std::remove_cvref_t<P>, ProviderStorage>)
            && provides<std::remove_cvref_t<P>, T, HasContext, Args...>
            && std::constructible_from<std::remove_cvref_t<P>, P>
    ProviderStorage(P&& source)
        : source_(std::make_shared<Model<std::remove_cvref_t<P>>>(
              std::forward<P>(source)))
    {}

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(source_);
    }

    [[nodiscard]] Result<T> provide(Args... args) const
    {
        if (!source_) {
            Diagnostic error;
            error.code = ErrorCode::no_provider;
            error.message = "This resource has no retained provider.";
            return std::unexpected(std::move(error));
        }
        return source_->provide(args...);
    }

private:
    struct Interface {
        virtual ~Interface() = default;
        [[nodiscard]] virtual Result<T> provide(Args... args) const = 0;
    };

    template<class P>
    struct Model final : Interface {
        template<class Source>
        explicit Model(Source&& source) : source_(std::forward<Source>(source)) {}

        [[nodiscard]] Result<T> provide(Args... args) const override
        {
            auto result = invoke_provider<HasContext>(source_, args...);
            if (!result) {
                return std::unexpected(to_diagnostic(std::move(result.error())));
            }
            if constexpr (std::is_void_v<T>) {
                return {};
            } else {
                return Result<T>{std::in_place, std::move(*result)};
            }
        }

        const P source_;
    };

    std::shared_ptr<const Interface> source_;
};

template<class T, class Context, class Request>
struct provider_signature {
    using type = ProviderStorage<T, true, Context&, const Request&>;

    template<class P>
    static constexpr bool accepts = provides<P, T, true, Context&, const Request&>;
};

template<class T, class Context>
struct provider_signature<T, Context, void> {
    using type = ProviderStorage<T, true, Context&>;

    template<class P>
    static constexpr bool accepts = provides<P, T, true, Context&>;
};

template<class T, class Request>
struct provider_signature<T, void, Request> {
    using type = ProviderStorage<T, false, const Request&>;

    template<class P>
    static constexpr bool accepts = provides<P, T, false, const Request&>;
};

template<class T>
struct provider_signature<T, void, void> {
    using type = ProviderStorage<T, false>;

    template<class P>
    static constexpr bool accepts = provides<P, T, false>;
};

} // namespace detail

template<class P, class T, class Context = void, class Request = void>
concept ProviderFor = detail::provider_signature<T, Context, Request>::template accepts<P>;

// Copies share the immutable recipe, never the product. Erasure is invoked
// only during provisioning, so ordinary resource access remains direct.
template<class T, class Context = void, class Request = void>
using Provider = typename detail::provider_signature<T, Context, Request>::type;

template<class P>
class SharedProvider final {
public:
    template<class Source>
        requires std::constructible_from<P, Source>
    explicit SharedProvider(Source&& source)
        : source_(std::make_shared<const P>(std::forward<Source>(source)))
    {}

    template<class... Args>
        requires requires(const P& source, Args&&... args) {
            resources::into_result(resources::provide(source, std::forward<Args>(args)...));
        }
    [[nodiscard]] auto provide(Args&&... args) const
    {
        using R = decltype(resources::into_result(
            resources::provide(*source_, std::forward<Args>(args)...)));
        if (!source_) {
            Diagnostic error;
            error.code = ErrorCode::no_provider;
            error.message = "This shared provider has been moved from.";
            return R{std::unexpected(std::move(error))};
        }
        return resources::into_result(
            resources::provide(*source_, std::forward<Args>(args)...));
    }

private:
    std::shared_ptr<const P> source_;
};

template<class P>
[[nodiscard]] auto share_provider(P&& source)
{
    return SharedProvider<std::decay_t<P>>{std::forward<P>(source)};
}

} // namespace vng::resources
