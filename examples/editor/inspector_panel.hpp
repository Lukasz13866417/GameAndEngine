#pragma once

#include <memory>
#include <string_view>
#include <vector>
#include <vng/editor/inspector.hpp>
#include <vng/ui/ui.hpp>

namespace editor_example {
// A UI-process adapter for owning protocol descriptions, not worker objects.
// Poll once after each Screen::update(). Returned actions carry the latest shown
// stamp; the host serializes delivery and acknowledges revisions before sending
// dependent actions. Layout and draft state stay entirely in the UI process.
class InspectorPanel final {
public:
    explicit InspectorPanel(vng::ui::Container parent);
    ~InspectorPanel();
    InspectorPanel(InspectorPanel&&) noexcept;
    InspectorPanel& operator=(InspectorPanel&&) noexcept;
    InspectorPanel(const InspectorPanel&) = delete;
    InspectorPanel& operator=(const InspectorPanel&) = delete;

    void show(const vng::editor::Schema& schema);
    void clear();
    [[nodiscard]] std::vector<vng::editor::Event> poll();
    struct NumberEdit {
        vng::f32 value{};
        bool pressed{}, changed{}, committed{};
    };
    // Read-only local interaction after poll(), for built-in properties whose
    // authoring/streaming belongs to the UI instead of a worker callback.
    [[nodiscard]] std::optional<NumberEdit> number_edit(std::string_view group, std::string_view field) const;
    void reset_number(std::string_view group, std::string_view field, vng::f32 value);
    [[nodiscard]] std::string_view status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace editor_example
