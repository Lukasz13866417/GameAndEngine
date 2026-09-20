#pragma once

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace vng::resources {

// A shared resource is a stable, read-only snapshot. It cannot publish a new
// native handle behind an owner's back; replacement belongs to the owner.
template<class T>
class Shared final {
public:
    template<class Value>
        requires std::constructible_from<T, Value>
    explicit Shared(Value&& value)
        : value_(std::make_shared<const T>(std::forward<Value>(value)))
    {}

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(value_);
    }

    [[nodiscard]] const T& get() const noexcept { return *value_; }
    [[nodiscard]] const T& operator*() const noexcept { return *value_; }
    [[nodiscard]] const T* operator->() const noexcept { return value_.get(); }

private:
    std::shared_ptr<const T> value_;
};

template<class T>
[[nodiscard]] auto share_resource(T&& resource)
{
    return Shared<std::decay_t<T>>{std::forward<T>(resource)};
}

// This transfer package carries an already-created product together with its
// reconstruction source. Construction does not call the provider or upload
// again. Only the receiving owner decides when and how to replace it.
template<class T, class P>
struct Provided final {
    T value;
    P provider;
};

template<class T, class P>
[[nodiscard]] auto provided(T&& resource, P&& source)
{
    return Provided<std::decay_t<T>, std::decay_t<P>>{
        std::forward<T>(resource), std::forward<P>(source)};
}

} // namespace vng::resources
