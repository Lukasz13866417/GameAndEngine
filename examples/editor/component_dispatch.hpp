#pragma once

#include <variant>

namespace editor_example {
// Optional entry point for components which friend Dispatcher. Runtime
// alternatives visit here; a parent which already knows the concrete situation
// can instead call its child's private handler through owner friendship.
// No base class or registry is needed.
struct Dispatcher final {
    template<class Component, class Situation, class Context>
        requires requires(Component& component, const Situation& situation, const Context& context) {
            component.handle(situation, context);
        }
    constexpr decltype(auto) operator()(Component& component, const Situation& situation,
                                       const Context& context) const {
        return component.handle(situation, context);
    }

    template<class Component, class... Situations, class Context>
        requires (requires(Component& component, const Situations& situation, const Context& context) {
            component.handle(situation, context);
        } && ...)
    constexpr decltype(auto) operator()(Component& component,
                                       const std::variant<Situations...>& situation,
                                       const Context& context) const {
        return std::visit([&](const auto& current) -> decltype(auto) {
            return (*this)(component, current, context);
        }, situation);
    }
};
inline constexpr Dispatcher dispatch{};
} // namespace editor_example
