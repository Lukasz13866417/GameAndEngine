#include "../../examples/editor/component_dispatch.hpp"
#include "../../examples/editor/component_debug.hpp"
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <memory>

namespace {
using namespace editor_example;
struct First { int value; };
struct Second { int value; };
struct Unknown {};
struct Context { int offset; };
class Component {
public:
    int calls{};
private:
    friend struct editor_example::Dispatcher;
    int handle(const First& s, const Context& c) { ++calls; return s.value + c.offset; }
    int handle(const Second& s, const Context& c) { ++calls; return s.value - c.offset; }
};
template<class T>
concept PublicHandler = requires(T& component, First first, Context context) { component.handle(first, context); };
template<class Situation>
concept Dispatchable = requires(Component& component, Situation situation, Context context) {
    dispatch(component, situation, context);
};
static_assert(!PublicHandler<Component>);
static_assert(Dispatchable<First>);
static_assert(Dispatchable<std::variant<First, Second>>);
static_assert(!Dispatchable<Unknown>);
static_assert(!Dispatchable<std::variant<First, Unknown>>);

class ReferenceComponent {
public:
    int value{};
private:
    friend struct editor_example::Dispatcher;
    int& handle(const First&, const Context&) { return value; }
    int& handle(const Second&, const Context&) { return value; }
};
}
TEST_CASE("Private handlers support concrete and exhaustive variant dispatch", "[editor][dispatch]") {
    Component component;
    CHECK(dispatch(component, First{10}, Context{2}) == 12);
    CHECK(component.calls == 1);
    std::variant<First, Second> situation = Second{10};
    CHECK(dispatch(component, situation, Context{2}) == 8);
    CHECK(component.calls == 2);
    situation = First{7};
    CHECK(dispatch(component, situation, Context{3}) == 10);
    CHECK(component.calls == 3);
}
TEST_CASE("Dispatch preserves references without copying a component", "[editor][dispatch]") {
    ReferenceComponent component;
    std::variant<First, Second> situation = First{0};
    static_assert(std::same_as<decltype(dispatch(component, situation, Context{})), int&>);
    dispatch(component, situation, Context{}) = 42;
    CHECK(component.value == 42);
}
TEST_CASE("Component reports own facts and aggregate only explicit children", "[editor][diagnostics]") {
    DebugReport report{.name="workspace", .role="editor", .situation="Mesh",
        .children={{.name="menu", .role="operations", .situation="Edges",
            .received={{"selected edges","2"}}, .observations={{"requested action","Subdivide"}}}}};
    const auto first = report.string();
    CHECK(first.find("workspace.menu") != std::string::npos);
    CHECK(first.find("selected edges: 2") != std::string::npos);
    CHECK(first.find("requested action: Subdivide") != std::string::npos);
    CHECK(first.find("worker progress") == std::string::npos);
    CHECK(report.string() == first);
}
