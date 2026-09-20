#pragma once

#include <concepts>
#include <type_traits>

namespace vng::render {

// A compile-time identity with just the backend's entry-point types. They may
// be forward-declared: naming a backend must not pull its implementation,
// window library, or native API headers into a portable algorithm.
template<class B>
concept Backend = std::same_as<B, std::remove_cvref_t<B>> && requires {
    typename B::device_type;
    typename B::frame_type;
} && std::is_class_v<typename B::device_type>
  && std::is_class_v<typename B::frame_type>;

template<class T>
using backend_t = typename std::remove_cvref_t<T>::backend_type;

template<class T>
concept BackendBound = requires { typename backend_t<T>; } && Backend<backend_t<T>>;

template<class A, class B>
concept SameBackend = BackendBound<A> && BackendBound<B>
    && std::same_as<backend_t<A>, backend_t<B>>;

} // namespace vng::render
