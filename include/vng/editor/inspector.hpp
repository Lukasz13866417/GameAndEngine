#pragma once

#include <concepts>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <vng/core/types.hpp>

namespace vng::editor {

struct Diagnostic final {
    std::string message;
    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};
template<class T> using Result = std::expected<T, Diagnostic>;

// These are protocol identities, never native object addresses. A new worker
// must use a new generation, even when recreating the same authored object.
struct Stamp final {
    u64 object{};
    u64 generation{};
    u64 revision{};
    // Optional inspection context identity (selection/playhead), independent
    // of authored revision. Stale callbacks cannot act on a newly viewed value.
    u64 context{};
    friend bool operator==(const Stamp&, const Stamp&) = default;
};

using Value = std::variant<bool, i32, u32, f32, Vec3, std::string>;
template<class T> concept EditableValue =
    std::same_as<T, bool> || std::same_as<T, i32> || std::same_as<T, u32> ||
    std::same_as<T, f32> || std::same_as<T, Vec3> || std::same_as<T, std::string>;

struct NamedValue final {
    std::string key;
    Value value;
    friend bool operator==(const NamedValue&, const NamedValue&) = default;
};

struct Field final {
    std::string key;
    std::string label;
    Value value;
    std::optional<f32> minimum;
    std::optional<f32> maximum;
    friend bool operator==(const Field&, const Field&) = default;
};

enum class Kind : u8 { group, action, translation_gizmo };
enum class Phase : u8 { apply, activate, begin, update, commit, cancel };

// An additional, bidirectional translation handle. Direction is in world
// space, need not be unit length, and is independent of the camera/backend.
struct TranslationAxis final {
    std::string label;
    Vec3 direction;
    friend bool operator==(const TranslationAxis&, const TranslationAxis&) = default;
};

struct Control final {
    std::string key;
    std::string label;
    Kind kind{Kind::group};
    std::vector<Field> fields;
    bool live{};
    std::string apply_label;
    std::vector<TranslationAxis> translation_axes{}; // supplements the usual world XYZ handles
    friend bool operator==(const Control&, const Control&) = default;
};

// Owning, serializable descriptions. No callback, pointer, native Settings
// layout, UI object, or backend object crosses the process boundary.
struct Schema final {
    Stamp stamp;
    std::vector<Control> controls;
    friend bool operator==(const Schema&, const Schema&) = default;
};

struct Event final {
    Stamp stamp;
    std::string control;
    Phase phase{Phase::apply};
    std::vector<NamedValue> values;
    friend bool operator==(const Event&, const Event&) = default;
};

[[nodiscard]] Result<void> validate(const Schema& schema);
[[nodiscard]] Result<void> validate(const Event& event);
// Bounded, versioned, little-endian wire payloads; strings may contain NUL
// bytes at the transport layer. Do not send these using strlen().
[[nodiscard]] Result<std::string> encode_schema(const Schema& schema);
[[nodiscard]] Result<Schema> decode_schema(std::string_view bytes);
[[nodiscard]] Result<std::string> encode_event(const Event& event);
[[nodiscard]] Result<Event> decode_event(std::string_view bytes);

namespace detail {
template<class T> struct Expected : std::false_type {};
template<class T, class E> struct Expected<std::expected<T, E>> : std::true_type {};

template<class Function, class... Args>
Result<void> invoke(Function& function, Args&&... args)
{
    using R = std::invoke_result_t<Function&, Args...>;
    if constexpr (std::same_as<R, void>) {
        std::invoke(function, std::forward<Args>(args)...);
        return {};
    } else {
        static_assert(Expected<R>::value,
            "An editor callback must return void or std::expected<void, Error>.");
        static_assert(std::same_as<typename R::value_type, void>);
        auto result = std::invoke(function, std::forward<Args>(args)...);
        if (result) return {};
        if constexpr (std::convertible_to<decltype(result.error()), std::string>)
            return std::unexpected(Diagnostic{std::string(result.error())});
        else {
            static_assert(requires { std::string(result.error().message); },
                "Callback errors must be strings or expose a string message.");
            return std::unexpected(Diagnostic{std::string(result.error().message)});
        }
    }
}

struct InspectorState;
using Handler = std::function<Result<void>(Phase, std::span<const NamedValue>)>;
std::size_t add_control(const std::shared_ptr<InspectorState>& state, Control control);
void add_field(const std::shared_ptr<InspectorState>& state, std::size_t index, Field field);
void set_handler(const std::shared_ptr<InspectorState>& state, std::size_t index,
    bool live, std::string apply_label, Handler handler);
void configure_gizmo(const std::shared_ptr<InspectorState>& state, std::size_t index,
    Vec3 initial, std::function<Result<void>(Vec3, Phase)> callback);
void add_translation_axis(const std::shared_ptr<InspectorState>&, std::size_t, TranslationAxis);
std::string label_for(std::string_view key);

template<class Settings> struct Binding final {
    explicit Binding(const Settings& value) : baseline(value) {}
    Settings baseline;
    std::vector<std::pair<std::string, std::function<void(Settings&, const Value&)>>> setters;
};
} // namespace detail

class TranslationGizmo final {
public:
    TranslationGizmo& axis(std::string label, Vec3 world_direction) {
        detail::add_translation_axis(state_, index_, {std::move(label), world_direction});
        return *this;
    }
private:
    TranslationGizmo(std::shared_ptr<detail::InspectorState> state, std::size_t index)
        : state_(std::move(state)), index_(index) {}
    std::shared_ptr<detail::InspectorState> state_;
    std::size_t index_{};
    friend class Inspector;
};

template<class Settings> class Edit final {
public:
    template<EditableValue T>
    Edit& field(std::string key, T Settings::* member, std::string label = {})
    { return add(std::move(key), member, {}, {}, std::move(label)); }

    template<class T> requires (std::same_as<T, f32> || std::same_as<T, i32> || std::same_as<T, u32>)
    Edit& slider(std::string key, T Settings::* member, f32 minimum, f32 maximum,
        std::string label = {})
    { return add(std::move(key), member, minimum, maximum, std::move(label)); }

    Edit& toggle(std::string key, bool Settings::* member, std::string label = {})
    { return field(std::move(key), member, std::move(label)); }

    template<class Function> void apply(std::string label, Function callback)
    { finish(false, std::move(label), std::move(callback)); }

    template<class Function> void live(Function callback)
    { finish(true, {}, std::move(callback)); }

private:
    Edit(std::shared_ptr<detail::InspectorState> state, std::size_t index, const Settings& value)
        : state_(std::move(state)), index_(index), binding_(std::make_shared<detail::Binding<Settings>>(value)) {}

    template<EditableValue T>
    Edit& add(std::string key, T Settings::* member, std::optional<f32> minimum,
        std::optional<f32> maximum, std::string label)
    {
        if (label.empty()) label = detail::label_for(key);
        detail::add_field(state_, index_, Field{key, std::move(label),
            binding_->baseline.*member, minimum, maximum});
        binding_->setters.emplace_back(std::move(key), [member](Settings& candidate, const Value& value) {
            candidate.*member = std::get<T>(value);
        });
        return *this;
    }

    template<class Function> void finish(bool live, std::string label, Function callback)
    {
        // std::function stores this shared callback wrapper, so callbacks may
        // themselves be move-only without leaking their type into the ABI.
        auto owned_callback = std::make_shared<Function>(std::move(callback));
        detail::set_handler(state_, index_, live, std::move(label),
            [binding = binding_, owned_callback](Phase, std::span<const NamedValue> values) -> Result<void> {
                Settings candidate = binding->baseline;
                for (const auto& value : values)
                    for (const auto& [key, setter] : binding->setters)
                        if (key == value.key) { setter(candidate, value.value); break; }
                auto applied = detail::invoke(*owned_callback, std::as_const(candidate));
                if (applied) binding->baseline = std::move(candidate);
                return applied;
            });
    }

    std::shared_ptr<detail::InspectorState> state_;
    std::size_t index_{};
    std::shared_ptr<detail::Binding<Settings>> binding_;
    friend class Inspector;
};

// Construct/destruct this alongside the inspected worker-side object. Its
// callbacks borrow objects exactly like ordinary C++ lambdas; a stamp rejects
// remote stale events, but cannot rescue a locally dangling [this] capture.
class Inspector final {
public:
    Inspector(u64 object, u64 generation, u64 revision = 0);
    explicit Inspector(Stamp stamp);
    Inspector(const Inspector&) = delete;
    Inspector& operator=(const Inspector&) = delete;
    Inspector(Inspector&&) noexcept = default;
    Inspector& operator=(Inspector&&) noexcept = default;

    template<class Settings> requires std::copy_constructible<Settings> && std::is_copy_assignable_v<Settings>
    [[nodiscard]] Edit<Settings> edit(std::string key, const Settings& value, std::string label = {})
    {
        if (label.empty()) label = detail::label_for(key);
        auto index = detail::add_control(state_, Control{std::move(key), std::move(label), Kind::group, {}, false, {}});
        return Edit<Settings>{state_, index, value};
    }

    template<class Function> void action(std::string key, Function callback, std::string label = {})
    {
        if (label.empty()) label = detail::label_for(key);
        auto index = detail::add_control(state_, Control{std::move(key), std::move(label), Kind::action, {}, false, {}});
        auto owned_callback = std::make_shared<Function>(std::move(callback));
        detail::set_handler(state_, index, false, {},
            [owned_callback](Phase, std::span<const NamedValue>) { return detail::invoke(*owned_callback); });
    }

    template<class Function>
    TranslationGizmo translation_gizmo(std::string key, Vec3 value, Function callback, std::string label = {})
    {
        if (label.empty()) label = detail::label_for(key);
        auto index = detail::add_control(state_, Control{std::move(key), std::move(label),
            Kind::translation_gizmo, {Field{"position", "Position", value, {}, {}}}, true, {}});
        auto owned_callback = std::make_shared<Function>(std::move(callback));
        detail::configure_gizmo(state_, index, value,
            [owned_callback](Vec3 next, Phase phase) -> Result<void> {
                if constexpr (std::invocable<Function&, Vec3, Phase>)
                    return detail::invoke(*owned_callback, next, phase);
                else {
                    if (phase == Phase::begin) return {};
                    return detail::invoke(*owned_callback, next);
                }
            });
        return TranslationGizmo{state_, index};
    }

    [[nodiscard]] const Schema& schema() const;
    // Each successful event advances the revision, including gesture markers.
    // Send the resulting schema/ack stamp before submitting dependent events.
    // Call at the worker update boundary, never concurrently with rendering.
    [[nodiscard]] Result<void> dispatch(const Event& event);

private:
    std::shared_ptr<detail::InspectorState> state_;
};

} // namespace vng::editor
