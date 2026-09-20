#include <vng/editor/inspector.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace vng::editor {
namespace {

constexpr std::size_t max_payload = 4U * 1024U * 1024U;
constexpr std::size_t max_string = 64U * 1024U;
constexpr std::size_t max_controls = 1024;
constexpr std::size_t max_fields = 256;
constexpr std::size_t max_translation_axes = 8;

Result<void> failure(std::string message)
{ return std::unexpected(Diagnostic{std::move(message)}); }

bool valid_utf8(std::string_view text)
{
    std::size_t at = 0;
    while (at < text.size()) {
        auto first = static_cast<u8>(text[at++]);
        if (first < 0x80U) { if (first == 0) return false; continue; }
        unsigned count{}; u32 code{}, minimum{};
        if (first >= 0xC2U && first <= 0xDFU) { count = 1; code = first & 0x1FU; minimum = 0x80U; }
        else if (first >= 0xE0U && first <= 0xEFU) { count = 2; code = first & 0x0FU; minimum = 0x800U; }
        else if (first >= 0xF0U && first <= 0xF4U) { count = 3; code = first & 0x07U; minimum = 0x10000U; }
        else return false;
        if (text.size() - at < count) return false;
        for (unsigned i = 0; i < count; ++i) {
            auto next = static_cast<u8>(text[at++]);
            if ((next & 0xC0U) != 0x80U) return false;
            code = (code << 6U) | (next & 0x3FU);
        }
        if (code < minimum || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) return false;
    }
    return true;
}

bool valid_text(std::string_view text) { return text.size() <= max_string && valid_utf8(text); }
bool valid_key(std::string_view key)
{
    return !key.empty() && key.size() <= 128 && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
    });
}

bool valid_value(const Value& value)
{
    return std::visit([](const auto& item) {
        using T = std::remove_cvref_t<decltype(item)>;
        if constexpr (std::same_as<T, f32>) return std::isfinite(item);
        else if constexpr (std::same_as<T, Vec3>)
            return std::isfinite(item.x) && std::isfinite(item.y) && std::isfinite(item.z);
        else if constexpr (std::same_as<T, std::string>) return valid_text(item);
        else return true;
    }, value);
}

bool valid_axis(const TranslationAxis& axis) {
    return !axis.label.empty() && valid_text(axis.label) && valid_value(axis.direction) &&
        std::hypot(static_cast<f64>(axis.direction.x), static_cast<f64>(axis.direction.y),
                   static_cast<f64>(axis.direction.z)) > 1e-6;
}

Result<void> validate_field(const Field& field)
{
    if (!valid_key(field.key)) return failure("Invalid editor field key: " + field.key);
    if (!valid_text(field.label) || !valid_value(field.value)) return failure("Invalid editor field value or label: " + field.key);
    if (field.minimum.has_value() != field.maximum.has_value()) return failure("Editor slider requires both bounds: " + field.key);
    if (field.minimum) {
        if (!std::isfinite(*field.minimum) || !std::isfinite(*field.maximum) || *field.minimum > *field.maximum)
            return failure("Invalid editor slider range: " + field.key);
        if (field.value.index() < 1 || field.value.index() > 3)
            return failure("Only numeric scalar fields can have slider bounds: " + field.key);
    }
    return {};
}

Result<void> validate_patch(const Field& field, const Value& value)
{
    if (field.value.index() != value.index()) return failure("Wrong value type for editor field: " + field.key);
    if (!valid_value(value)) return failure("Invalid value for editor field: " + field.key);
    if (field.minimum) {
        const auto in_range = std::visit([&](const auto& item) {
            using T = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::same_as<T, i32> || std::same_as<T, u32> || std::same_as<T, f32>)
                return static_cast<double>(item) >= static_cast<double>(*field.minimum) &&
                    static_cast<double>(item) <= static_cast<double>(*field.maximum);
            else return false;
        }, value);
        if (!in_range) return failure("Value outside editor slider range: " + field.key);
    }
    return {};
}

// Decoding uses a local exception only for bounded-parser control flow, never
// for invoking user code. We validate counts before allocating their storage.
struct WireFailure final { std::string message; };
struct Writer final {
    std::string bytes;
    void byte(u8 value) { bytes.push_back(static_cast<char>(value)); check(); }
    void check() { if (bytes.size() > max_payload) throw WireFailure{"Editor payload exceeds 4 MiB"}; }
    void integer(u64 value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) byte(static_cast<u8>((value >> (i * 8U)) & 0xFFU));
    }
    void number(f32 value) { integer(std::bit_cast<u32>(value), 4); }
    void string(std::string_view value) {
        integer(static_cast<u32>(value.size()), 4); bytes.append(value); check();
    }
    void stamp(Stamp value) { integer(value.object, 8); integer(value.generation, 8); integer(value.revision, 8); integer(value.context, 8); }
    void value(const Value& value) {
        byte(static_cast<u8>(value.index()));
        std::visit([&](const auto& item) {
            using T = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::same_as<T, bool>) byte(item ? 1 : 0);
            else if constexpr (std::same_as<T, i32>) integer(std::bit_cast<u32>(item), 4);
            else if constexpr (std::same_as<T, u32>) integer(item, 4);
            else if constexpr (std::same_as<T, f32>) number(item);
            else if constexpr (std::same_as<T, Vec3>) { number(item.x); number(item.y); number(item.z); }
            else string(item);
        }, value);
    }
};

struct Reader final {
    std::string_view bytes;
    std::size_t at{};
    u8 byte() {
        if (at == bytes.size()) throw WireFailure{"Truncated editor payload"};
        return static_cast<u8>(bytes[at++]);
    }
    u64 integer(unsigned size) {
        u64 value{};
        for (unsigned i = 0; i < size; ++i) value |= static_cast<u64>(byte()) << (i * 8U);
        return value;
    }
    u32 count(std::size_t maximum) {
        const auto value = static_cast<u32>(integer(4));
        if (value > maximum) throw WireFailure{"Editor payload collection limit exceeded"};
        return value;
    }
    bool boolean() {
        auto value = byte();
        if (value > 1) throw WireFailure{"Invalid editor wire boolean"};
        return value != 0;
    }
    f32 number() { return std::bit_cast<f32>(static_cast<u32>(integer(4))); }
    std::string string() {
        auto size = count(max_string);
        if (size > bytes.size() - at) throw WireFailure{"Truncated editor string"};
        std::string result{bytes.substr(at, size)}; at += size; return result;
    }
    Stamp stamp() { return {integer(8), integer(8), integer(8), integer(8)}; }
    Value value() {
        switch (byte()) {
        case 0: return boolean();
        case 1: return std::bit_cast<i32>(static_cast<u32>(integer(4)));
        case 2: return static_cast<u32>(integer(4));
        case 3: return number();
        case 4: return Vec3{number(), number(), number()};
        case 5: return string();
        default: throw WireFailure{"Unknown editor wire value type"};
        }
    }
    void header(std::string_view expected) {
        if (bytes.size() > max_payload) throw WireFailure{"Editor payload exceeds 4 MiB"};
        if (!bytes.starts_with(expected)) throw WireFailure{"Unknown editor payload kind or version"};
        at = expected.size();
    }
    void end() { if (at != bytes.size()) throw WireFailure{"Trailing editor payload data"}; }
};

template<class T, class Function> Result<T> wire(Function function)
{
    try { return function(); }
    catch (const WireFailure& error) { return std::unexpected(Diagnostic{error.message}); }
    catch (const std::exception& error) { return std::unexpected(Diagnostic{error.what()}); }
}

} // namespace

Result<void> validate(const Schema& schema)
{
    if (schema.controls.size() > max_controls) return failure("Too many editor controls");
    std::unordered_set<std::string_view> controls;
    for (const auto& control : schema.controls) {
        if (!valid_key(control.key) || !controls.insert(control.key).second)
            return failure("Invalid or duplicate editor control key: " + control.key);
        if (!valid_text(control.label) || !valid_text(control.apply_label)) return failure("Invalid editor control label");
        if (static_cast<u8>(control.kind) > static_cast<u8>(Kind::translation_gizmo)) return failure("Unknown editor control kind");
        if (control.fields.size() > max_fields) return failure("Too many fields in editor control: " + control.key);
        std::unordered_set<std::string_view> fields;
        for (const auto& field : control.fields) {
            if (!fields.insert(field.key).second) return failure("Duplicate editor field key: " + field.key);
            if (auto valid = validate_field(field); !valid) return valid;
            if (auto valid = validate_patch(field, field.value); !valid) return valid;
        }
        if (control.kind == Kind::group && (!control.live && control.apply_label.empty()))
            return failure("Editor edit group needs apply() or live(): " + control.key);
        if (control.kind == Kind::action && (!control.fields.empty() || control.live || !control.apply_label.empty()))
            return failure("Editor actions cannot contain fields or edit policies");
        if (control.kind == Kind::translation_gizmo &&
            (control.fields.size() != 1 || control.fields[0].key != "position" ||
             !std::holds_alternative<Vec3>(control.fields[0].value) || !control.live || !control.apply_label.empty()))
            return failure("Translation gizmos require one Vec3 position field");
        if (control.translation_axes.size() > max_translation_axes ||
            (control.kind != Kind::translation_gizmo && !control.translation_axes.empty()))
            return failure("Extra translation axes require a translation gizmo (maximum eight)");
        std::unordered_set<std::string_view> axes;
        for (const auto& axis : control.translation_axes)
            if (!valid_axis(axis) || !axes.insert(axis.label).second)
                return failure("Invalid or duplicate translation axis: " + axis.label);
    }
    return {};
}

Result<void> validate(const Event& event)
{
    if (!valid_key(event.control)) return failure("Invalid editor event control key");
    if (static_cast<u8>(event.phase) > static_cast<u8>(Phase::cancel)) return failure("Unknown editor event phase");
    if (event.values.size() > max_fields) return failure("Too many editor event values");
    std::unordered_set<std::string_view> keys;
    for (const auto& value : event.values) {
        if (!valid_key(value.key) || !keys.insert(value.key).second)
            return failure("Invalid or duplicate editor event field: " + value.key);
        if (!valid_value(value.value)) return failure("Invalid editor event value: " + value.key);
    }
    return {};
}

Result<std::string> encode_schema(const Schema& schema)
{
    if (auto valid = validate(schema); !valid) return std::unexpected(valid.error());
    return wire<std::string>([&] {
        Writer out{{"VNGS\x03", 5}}; out.stamp(schema.stamp);
        out.integer(static_cast<u32>(schema.controls.size()), 4);
        for (const auto& control : schema.controls) {
            out.string(control.key); out.string(control.label); out.byte(static_cast<u8>(control.kind));
            out.byte(control.live ? 1 : 0); out.string(control.apply_label);
            out.integer(static_cast<u32>(control.fields.size()), 4);
            for (const auto& field : control.fields) {
                out.string(field.key); out.string(field.label); out.value(field.value);
                out.byte(field.minimum ? 1 : 0);
                if (field.minimum) { out.number(*field.minimum); out.number(*field.maximum); }
            }
            out.integer(static_cast<u32>(control.translation_axes.size()), 4);
            for (const auto& axis : control.translation_axes) {
                out.string(axis.label);
                out.number(axis.direction.x); out.number(axis.direction.y); out.number(axis.direction.z);
            }
        }
        return std::move(out.bytes);
    });
}

Result<Schema> decode_schema(std::string_view bytes)
{
    return wire<Schema>([&]() -> Result<Schema> {
        Reader in{bytes}; in.header({"VNGS\x03", 5}); Schema schema{in.stamp(), {}};
        auto count = in.count(max_controls);
        for (u32 i = 0; i < count; ++i) {
            Control control;
            control.key = in.string(); control.label = in.string(); control.kind = static_cast<Kind>(in.byte());
            control.live = in.boolean(); control.apply_label = in.string();
            auto fields = in.count(max_fields);
            for (u32 f = 0; f < fields; ++f) {
                Field field{in.string(), in.string(), in.value(), {}, {}};
                if (in.boolean()) { field.minimum = in.number(); field.maximum = in.number(); }
                control.fields.push_back(std::move(field));
            }
            const auto axes = in.count(max_translation_axes);
            for (u32 a = 0; a < axes; ++a)
                control.translation_axes.push_back({in.string(), {in.number(), in.number(), in.number()}});
            schema.controls.push_back(std::move(control));
        }
        in.end();
        if (auto valid = validate(schema); !valid) return std::unexpected(valid.error());
        return schema;
    });
}

Result<std::string> encode_event(const Event& event)
{
    if (auto valid = validate(event); !valid) return std::unexpected(valid.error());
    return wire<std::string>([&] {
        Writer out{{"VNGE\x02", 5}}; out.stamp(event.stamp); out.string(event.control);
        out.byte(static_cast<u8>(event.phase)); out.integer(static_cast<u32>(event.values.size()), 4);
        for (const auto& value : event.values) { out.string(value.key); out.value(value.value); }
        return std::move(out.bytes);
    });
}

Result<Event> decode_event(std::string_view bytes)
{
    return wire<Event>([&]() -> Result<Event> {
        Reader in{bytes}; in.header({"VNGE\x02", 5}); Event event{in.stamp(), in.string(), static_cast<Phase>(in.byte()), {}};
        auto count = in.count(max_fields);
        for (u32 i = 0; i < count; ++i) event.values.push_back({in.string(), in.value()});
        in.end();
        if (auto valid = validate(event); !valid) return std::unexpected(valid.error());
        return event;
    });
}

namespace detail {
struct Gizmo final {
    Vec3 original{};
    bool active{};
    std::function<Result<void>(Vec3, Phase)> callback;
};
struct Entry final { Handler handler; std::optional<Gizmo> gizmo; };
struct InspectorState final {
    Schema schema;
    std::vector<Entry> entries;
    bool dispatched{};
    bool invoking{};
};

std::string label_for(std::string_view key)
{
    std::string result{key}; bool uppercase = true;
    for (char& c : result) {
        if (c == '_' || c == '-' || c == '/') { c = ' '; uppercase = true; }
        else if (uppercase) { if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); uppercase = false; }
    }
    return result;
}

void ensure_building(const std::shared_ptr<InspectorState>& state)
{
    if (!state) throw std::logic_error("Cannot use a moved-from editor inspector");
    if (state->dispatched || state->invoking) throw std::logic_error("Editor descriptions cannot change after dispatch; rebuild the inspector");
}

std::size_t add_control(const std::shared_ptr<InspectorState>& state, Control control)
{
    ensure_building(state);
    if (!valid_key(control.key) || !valid_text(control.label)) throw std::invalid_argument("Invalid editor control key or label");
    if (state->schema.controls.size() == max_controls) throw std::length_error("Too many editor controls");
    for (const auto& previous : state->schema.controls)
        if (previous.key == control.key) throw std::invalid_argument("Duplicate editor control key: " + control.key);
    auto index = state->schema.controls.size();
    state->entries.emplace_back();
    try { state->schema.controls.push_back(std::move(control)); }
    catch (...) { state->entries.pop_back(); throw; }
    return index;
}

void add_field(const std::shared_ptr<InspectorState>& state, std::size_t index, Field field)
{
    ensure_building(state);
    auto& control = state->schema.controls.at(index);
    if (state->entries.at(index).handler) throw std::logic_error("Add editor fields before apply() or live()");
    if (auto valid = validate_field(field); !valid) throw std::invalid_argument(valid.error().message);
    if (auto valid = validate_patch(field, field.value); !valid) throw std::invalid_argument(valid.error().message);
    for (const auto& previous : control.fields)
        if (previous.key == field.key) throw std::invalid_argument("Duplicate editor field key: " + field.key);
    if (control.fields.size() == max_fields) throw std::length_error("Too many editor fields");
    control.fields.push_back(std::move(field));
}

void set_handler(const std::shared_ptr<InspectorState>& state, std::size_t index,
    bool live, std::string apply_label, Handler handler)
{
    ensure_building(state);
    auto& entry = state->entries.at(index);
    if (entry.handler || entry.gizmo) throw std::logic_error("Editor control callback already registered");
    if (!valid_text(apply_label)) throw std::invalid_argument("Invalid editor Apply label");
    auto& control = state->schema.controls.at(index);
    if (control.kind == Kind::group && !live && apply_label.empty()) throw std::invalid_argument("Apply label cannot be empty");
    control.live = live; control.apply_label = std::move(apply_label); entry.handler = std::move(handler);
}

void configure_gizmo(const std::shared_ptr<InspectorState>& state, std::size_t index,
    Vec3 initial, std::function<Result<void>(Vec3, Phase)> callback)
{
    ensure_building(state);
    if (!valid_value(initial)) throw std::invalid_argument("Invalid editor gizmo position");
    state->entries.at(index).gizmo = Gizmo{initial, false, std::move(callback)};
}

void add_translation_axis(const std::shared_ptr<InspectorState>& state, std::size_t index,
                          TranslationAxis axis) {
    ensure_building(state);
    if (!valid_axis(axis)) throw std::invalid_argument("Translation axis requires a label and a finite nonzero direction");
    auto& control = state->schema.controls.at(index);
    if (control.kind != Kind::translation_gizmo) throw std::logic_error("Not a translation gizmo");
    if (control.translation_axes.size() == max_translation_axes) throw std::length_error("Too many translation axes");
    if (std::ranges::find(control.translation_axes, axis.label, &TranslationAxis::label) != control.translation_axes.end())
        throw std::invalid_argument("Duplicate translation axis: " + axis.label);
    control.translation_axes.push_back(std::move(axis));
}
} // namespace detail

Inspector::Inspector(u64 object, u64 generation, u64 revision) : Inspector(Stamp{object, generation, revision}) {}
Inspector::Inspector(Stamp stamp) : state_(std::make_shared<detail::InspectorState>()) { state_->schema.stamp = stamp; }
const Schema& Inspector::schema() const
{
    if (!state_) throw std::logic_error("Cannot use a moved-from editor inspector");
    return state_->schema;
}

Result<void> Inspector::dispatch(const Event& event)
{
    if (!state_) return failure("Cannot dispatch to a moved-from editor inspector");
    if (state_->invoking) return failure("Reentrant editor dispatch is not allowed");
    if (event.stamp != state_->schema.stamp) return failure("Stale editor event: object, worker generation, or revision does not match");
    if (state_->schema.stamp.revision == std::numeric_limits<u64>::max()) return failure("Editor revision exhausted");
    if (auto valid = validate(event); !valid) return valid;
    if (auto valid = validate(state_->schema); !valid) return valid;
    auto found = std::ranges::find(state_->schema.controls, event.control, &Control::key);
    if (found == state_->schema.controls.end()) return failure("Unknown editor control: " + event.control);
    auto index = static_cast<std::size_t>(found - state_->schema.controls.begin());
    auto& control = *found;
    auto& entry = state_->entries[index];
    for (const auto& value : event.values) {
        auto field = std::ranges::find(control.fields, value.key, &Field::key);
        if (field == control.fields.end()) return failure("Unknown editor field: " + value.key);
        if (auto valid = validate_patch(*field, value.value); !valid) return valid;
    }

    // Dispatch cannot rebuild its own registry: vector references and callback
    // captures must remain stable until the call returns.
    struct Invocation final {
        detail::InspectorState& state;
        explicit Invocation(detail::InspectorState& value) : state(value) { state.invoking = true; }
        ~Invocation() { state.invoking = false; }
    } invocation{*state_};
    try {
        if (control.kind == Kind::translation_gizmo) {
            if (!entry.gizmo) return failure("Editor gizmo has no callback");
            auto& gizmo = *entry.gizmo;
            Vec3 next = std::get<Vec3>(control.fields[0].value);
            if (!event.values.empty()) next = std::get<Vec3>(event.values[0].value);
            switch (event.phase) {
            case Phase::begin:
                if (gizmo.active || !event.values.empty()) return failure("Gizmo begin requires an idle gesture and no values");
                break;
            case Phase::update:
                if (!gizmo.active || event.values.empty()) return failure("Gizmo update requires an active gesture and position");
                break;
            case Phase::commit:
                if (!gizmo.active) return failure("Gizmo commit requires an active gesture");
                break;
            case Phase::cancel:
                if (!gizmo.active || !event.values.empty()) return failure("Gizmo cancel requires an active gesture and no values");
                next = gizmo.original;
                break;
            case Phase::apply:
                if (gizmo.active || event.values.empty()) return failure("Gizmo apply requires an idle gesture and position");
                break;
            default: return failure("Unsupported gizmo event phase");
            }
            if (auto result = gizmo.callback(next, event.phase); !result) return result;
            if (event.phase == Phase::begin) { gizmo.original = next; gizmo.active = true; }
            if (event.phase == Phase::commit || event.phase == Phase::cancel) gizmo.active = false;
            control.fields[0].value = next;
        } else {
            if (!entry.handler) return failure("Editor control has no callback");
            if (control.kind == Kind::action) {
                if (event.phase != Phase::activate || !event.values.empty()) return failure("Action requires activate with no values");
            } else if (event.phase != Phase::apply &&
                !(control.live && (event.phase == Phase::update || event.phase == Phase::commit)))
                return failure("Unsupported edit group event phase");
            if (auto result = entry.handler(event.phase, event.values); !result) return result;
            for (const auto& value : event.values)
                std::ranges::find(control.fields, value.key, &Field::key)->value = value.value;
        }
        ++state_->schema.stamp.revision;
        state_->dispatched = true;
        return {};
    } catch (const std::exception& error) {
        return failure("Editor callback failed: " + std::string(error.what()));
    } catch (...) {
        return failure("Editor callback failed with a non-standard exception");
    }
}

} // namespace vng::editor
