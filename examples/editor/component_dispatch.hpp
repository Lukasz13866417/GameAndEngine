#pragma once

#include <variant>

namespace editor_example {
// The only caller of a component's private situation handlers. Concrete
// situations use ordinary overload resolution; runtime alternatives visit here,
// never in component-specific routing code. No base class or registry is needed.
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
