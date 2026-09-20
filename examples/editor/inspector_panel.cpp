#include "inspector_panel.hpp"
#include "number_control.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>

namespace editor_example {
namespace {
using namespace vng;
struct VectorFields {
    ui::Container row;
    std::array<ui::Container, 3> columns;
    std::array<ui::TextField, 3> components;
    void layout() {
        const auto width = std::max(0.0F, (row.bounds().width - 8) / 3);
        for (auto& column : columns) column.width(width);
    }
};
using Widget = std::variant<ui::Checkbox, NumberControl, ui::TextField, VectorFields>;
bool multiline(const editor::Value& value) {
    const auto* text = std::get_if<std::string>(&value);
    return text && text->find('\n') != std::string::npos;
}

template <class T> std::string format(T value) {
    std::array<char, 64> storage{};
    const auto [end, error] = std::to_chars(storage.data(), storage.data() + storage.size(), value);
    if (error != std::errc{})
        throw std::runtime_error("Cannot format inspector number");
    return {storage.data(), end};
}
std::string text(const editor::Value& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, std::string>)
                return v;
            else if constexpr (std::same_as<T, Vec3> || std::same_as<T, bool>)
                return {};
            else
                return format(v);
        },
        value);
}
template <class T> editor::Result<T> number(std::string_view value, std::string_view name) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    if (!value.empty() && value.front() == '+')
        value.remove_prefix(1);
    T result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() ||
        (std::floating_point<T> && !std::isfinite(static_cast<double>(result))))
        return std::unexpected(editor::Diagnostic{"Enter a valid number for " + std::string(name)});
    return result;
}
bool same_shape(const editor::Schema& a, const editor::Schema& b) {
    if (a.stamp.object != b.stamp.object || a.stamp.generation != b.stamp.generation ||
        a.controls.size() != b.controls.size())
        return false;
    for (std::size_t i = 0; i < a.controls.size(); ++i) {
        const auto& x = a.controls[i];
        const auto& y = b.controls[i];
        if (x.key != y.key || x.label != y.label || x.kind != y.kind || x.live != y.live ||
            x.apply_label != y.apply_label || x.fields.size() != y.fields.size())
            return false;
        for (std::size_t j = 0; j < x.fields.size(); ++j) {
            const auto& p = x.fields[j];
            const auto& q = y.fields[j];
            if (p.key != q.key || p.label != q.label || p.value.index() != q.value.index() ||
                p.minimum != q.minimum || p.maximum != q.maximum ||
                multiline(p.value) != multiline(q.value))
                return false;
        }
    }
    return true;
}
struct FieldView {
    editor::Field field;
    Widget widget;

    editor::Result<editor::Value> read() const {
        editor::Result<editor::Value> result = std::visit(
            [&](const auto& w) -> editor::Result<editor::Value> {
                using T = std::decay_t<decltype(w)>;
                if constexpr (std::same_as<T, ui::Checkbox>)
                    return w.value();
                else if constexpr (std::same_as<T, NumberControl>) {
                    const auto value = w.read();
                    if (!value)
                        return std::unexpected(editor::Diagnostic{value.error().message});
                    return *value;
                }
                else if constexpr (std::same_as<T, VectorFields>) {
                    Vec3 value{};
                    for (std::size_t i = 0; i < 3; ++i) {
                        auto component = number<f32>(w.components[i].getText(), field.label);
                        if (!component)
                            return std::unexpected(component.error());
                        value[static_cast<u32>(i)] = *component;
                    }
                    return value;
                } else {
                    return std::visit(
                        [&](const auto& initial) -> editor::Result<editor::Value> {
                            using V = std::decay_t<decltype(initial)>;
                            if constexpr (std::same_as<V, std::string>)
                                return std::string(w.getText());
                            else if constexpr (std::same_as<V, f32> || std::same_as<V, i32> ||
                                               std::same_as<V, u32>) {
                                auto parsed = number<V>(w.getText(), field.label);
                                if (!parsed)
                                    return std::unexpected(parsed.error());
                                return *parsed;
                            } else
                                return std::unexpected(
                                    editor::Diagnostic{"Unsupported numeric field"});
                        },
                        field.value);
                }
            },
            widget);
        if (!result || !field.minimum)
            return result;
        const bool in_range = std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::same_as<T, f32> || std::same_as<T, i32> || std::same_as<T, u32>)
                    return static_cast<double>(v) >= static_cast<double>(*field.minimum) &&
                           static_cast<double>(v) <= static_cast<double>(*field.maximum);
                else
                    return true;
            },
            *result);
        if (!in_range)
            return std::unexpected(editor::Diagnostic{field.label + " must be between " +
                                                      format(*field.minimum) + " and " +
                                                      format(*field.maximum)});
        return result;
    }
    void set(const editor::Value& value, bool acknowledged = false) {
        std::visit(
            [&](auto& w) {
                using T = std::decay_t<decltype(w)>;
                if constexpr (std::same_as<T, ui::Checkbox>) {
                    if (w.value() != std::get<bool>(value))
                        w.value(std::get<bool>(value));
                } else if constexpr (std::same_as<T, NumberControl>) {
                    // Apply can submit a valid text draft without a field-level
                    // Enter. Once that exact draft is acknowledged it is a new
                    // baseline, not an unsent edit to preserve indefinitely.
                    if (acknowledged)
                        w.reset(std::get<f32>(value));
                    else if (w.value() != std::get<f32>(value))
                        w.value(std::get<f32>(value));
                } else if constexpr (std::same_as<T, VectorFields>) {
                    const auto v = std::get<Vec3>(value);
                    for (u32 i = 0; i < 3; ++i) {
                        const auto formatted = format(v[i]);
                        if (w.components[i].getText() != formatted)
                            w.components[i].value(formatted);
                    }
                } else {
                    const auto formatted = text(value);
                    if (w.getText() != formatted)
                        w.value(formatted);
                }
            },
            widget);
    }
    bool committed() const {
        return std::visit(
            [](const auto& w) {
                using T = std::decay_t<decltype(w)>;
                if constexpr (std::same_as<T, ui::Checkbox>)
                    return w.changedValue().has_value();
                else if constexpr (std::same_as<T, NumberControl>)
                    return w.editCommitted();
                else if constexpr (std::same_as<T, VectorFields>)
                    return std::ranges::any_of(
                        w.components, [](const auto& c) { return c.submittedText().has_value(); });
                else
                    return w.submittedText().has_value();
            },
            widget);
    }
    void poll(std::string& message) {
        if (auto* vector = std::get_if<VectorFields>(&widget)) vector->layout();
        if (auto* number = std::get_if<NumberControl>(&widget)) {
            number->poll();
            if (number->editCommitted() || number->changedValue())
                message.clear();
            if (!number->status().empty())
                message = number->status();
        }
    }
};
struct ControlView {
    editor::Control control;
    std::vector<FieldView> fields;
    ui::Button button;
    std::vector<editor::NamedValue> submitted;
};
} // namespace

struct InspectorPanel::Impl {
    vng::ui::Container parent, body;
    std::optional<vng::editor::Schema> schema;
    std::vector<ControlView> controls;
    std::string message;

    explicit Impl(vng::ui::Container parent_) : parent(parent_) {}
    ~Impl() {
        try {
            if (body.valid())
                body.remove();
        } catch (...) {
        }
    }
    void reset() {
        if (body.valid())
            body.remove();
        body = {};
        controls.clear();
        schema.reset();
    }
    FieldView field(vng::ui::Container container, const vng::editor::Field& description) {
        using namespace vng;
        Widget widget;
        if (std::holds_alternative<bool>(description.value))
            widget = container.checkbox(description.label);
        else if (std::holds_alternative<f32>(description.value) && description.minimum &&
                 *description.minimum < *description.maximum &&
                 std::isfinite(*description.maximum - *description.minimum))
            widget = NumberControl{container, description.label, *description.minimum,
                                   *description.maximum, std::get<f32>(description.value)};
        else if (std::holds_alternative<Vec3>(description.value)) {
            // A vector field owns its label and components. Multiple XYZ fields
            // in one transform group remain distinct for navigation/automation.
            auto field = container.column().padding(0).gap(4);
            field.label(description.label).height(24);
            auto row = field.row().padding(0).gap(4);
            VectorFields vector;
            vector.row = row;
            const std::array axes{"X", "Y", "Z"};
            for (std::size_t i = 0; i < 3; ++i) {
                auto column = row.column().width(0).padding(0).gap(2);
                vector.columns[i] = column;
                column.label(axes[i]).height(20);
                vector.components[i] = column.text_input(axes[i]);
            }
            widget = vector;
        } else {
            container.label(description.label).height(24);
            widget = multiline(description.value)
                         ? container.text_area(description.label).height(96)
                         : container.text_input(description.label);
        }
        FieldView view{description, std::move(widget)};
        view.set(description.value);
        return view;
    }
    void rebuild(const vng::editor::Schema& next) {
        reset();
        body = parent.column().padding(0).gap(10);
        for (const auto& control : next.controls) {
            ControlView view;
            view.control = control;
            if (control.kind == vng::editor::Kind::action) {
                view.button = body.button(control.label);
            } else {
                auto group = body.column().padding(6).gap(6);
                group.label(control.label).height(28);
                for (const auto& description : control.fields)
                    view.fields.push_back(field(group, description));
                if (!control.live || control.kind == vng::editor::Kind::translation_gizmo)
                    view.button = group.button(control.apply_label.empty()
                        ? (control.kind == editor::Kind::translation_gizmo ? "Apply " + control.label : "Apply")
                        : control.apply_label);
            }
            controls.push_back(std::move(view));
        }
        schema = next;
        // Adding later groups can introduce a scrollbar and shrink all prior
        // rows. Reflow XYZ fields against the final content width, not the
        // pre-scrollbar width observed while constructing the first group.
        for (auto& control : controls)
            for (auto& field : control.fields)
                if (auto* vector = std::get_if<VectorFields>(&field.widget)) vector->layout();
    }
};
InspectorPanel::InspectorPanel(vng::ui::Container parent) : impl_(std::make_unique<Impl>(parent)) {
}
InspectorPanel::~InspectorPanel() = default;
InspectorPanel::InspectorPanel(InspectorPanel&&) noexcept = default;
InspectorPanel& InspectorPanel::operator=(InspectorPanel&&) noexcept = default;
void InspectorPanel::clear() {
    if (!impl_)
        return;
    impl_->reset();
    impl_->message.clear();
}
void InspectorPanel::show(const vng::editor::Schema& next) {
    if (!impl_)
        return;
    auto& p = *impl_;
    if (auto valid = vng::editor::validate(next); !valid) {
        p.message = valid.error().message;
        return;
    }
    // A repeated description is not an acknowledgement of an outstanding
    // submission. Keep its baseline and drafts until an actual revision arrives.
    if (p.schema && *p.schema == next)
        return;
    for (const auto& control : next.controls) {
        for (const auto& field : control.fields) {
            if (const auto* text = std::get_if<std::string>(&field.value);
                text && std::ranges::any_of(*text, [](unsigned char c) {
                    return (c < 32 && c != '\n') || c == 127;
                })) {
                p.message =
                    field.label + " contains control characters unsupported by this text inspector";
                return;
            }
            if (!field.minimum)
                continue;
            const bool valid = std::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::same_as<T, vng::f32> || std::same_as<T, vng::i32> ||
                                  std::same_as<T, vng::u32>)
                        return static_cast<double>(value) >= static_cast<double>(*field.minimum) &&
                               static_cast<double>(value) <= static_cast<double>(*field.maximum);
                    else
                        return false;
                },
                field.value);
            if (!valid) {
                p.message = field.label + " has an out-of-range value in its description";
                return;
            }
        }
    }
    try {
        if (!p.schema || !same_shape(*p.schema, next)) {
            p.rebuild(next);
        } else {
            for (std::size_t i = 0; i < p.controls.size(); ++i) {
                auto& control = p.controls[i];
                for (std::size_t j = 0; j < control.fields.size(); ++j) {
                    auto& field = control.fields[j];
                    const auto current = field.read();
                    const auto submitted =
                        std::ranges::find_if(control.submitted, [&](const auto& value) {
                            return value.key == field.field.key;
                        });
                    // An ack may change one group while another has an unsent
                    // draft. It must not erase that draft or half-typed number.
                    const bool acknowledged = current && submitted != control.submitted.end() &&
                                              *current == submitted->value;
                    const bool clean = current && (*current == field.field.value || acknowledged);
                    if (clean)
                        field.set(next.controls[i].fields[j].value, acknowledged);
                    field.field = next.controls[i].fields[j];
                }
                control.control = next.controls[i];
                control.submitted.clear();
            }
            p.schema = next;
        }
        p.message.clear();
    } catch (const std::exception& error) {
        p.message = error.what();
    }
}
std::vector<vng::editor::Event> InspectorPanel::poll() {
    using namespace vng;
    std::vector<editor::Event> result;
    if (!impl_ || !impl_->schema)
        return result;
    auto& p = *impl_;
    for (auto& control : p.controls) {
        for (auto& field : control.fields)
            field.poll(p.message);
        const bool live = control.control.live && control.control.kind == editor::Kind::group;
        if (!control.button.clicked() &&
            !(live && std::ranges::any_of(control.fields,
                                          [](const auto& field) { return field.committed(); })))
            continue;
        editor::Event event{p.schema->stamp,
                            control.control.key,
                            control.control.kind == editor::Kind::action ? editor::Phase::activate
                                                                         : editor::Phase::apply,
                            {}};
        bool valid = true;
        for (const auto& field : control.fields) {
            auto value = field.read();
            if (!value) {
                p.message = value.error().message;
                valid = false;
                break;
            }
            event.values.push_back({field.field.key, std::move(*value)});
        }
        if (!valid)
            continue;
        if (auto checked = editor::validate(event); !checked) {
            p.message = checked.error().message;
            continue;
        }
        p.message.clear();
        control.submitted = event.values;
        result.push_back(std::move(event));
    }
    return result;
}
std::string_view InspectorPanel::status() const noexcept {
    return impl_ ? std::string_view{impl_->message} : std::string_view{};
}
std::optional<InspectorPanel::NumberEdit> InspectorPanel::number_edit(std::string_view group,std::string_view name) const {
    if(!impl_) return {};
    for(const auto& control:impl_->controls) {
        if(control.control.key!=group) continue;
        for(const auto& field:control.fields)
            if(field.field.key==name)
                if(const auto* number=std::get_if<NumberControl>(&field.widget))
                    return NumberEdit{number->value(),number->isPressed(),number->changedValue().has_value(),number->editCommitted()};
    }
    return {};
}
void InspectorPanel::reset_number(std::string_view group,std::string_view name,vng::f32 value) {
    if(!impl_) return;
    for(auto& control:impl_->controls) {
        if(control.control.key!=group) continue;
        for(auto& field:control.fields)
            if(field.field.key==name)
                if(auto* number=std::get_if<NumberControl>(&field.widget)) number->reset(value);
    }
}
} // namespace editor_example
