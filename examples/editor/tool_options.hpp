#pragma once
#include <vng/editor/inspector.hpp>
#include <string_view>

namespace editor_example {
// Optional UI capability shared by gestures, one-shot operations and custom
// blueprint tools. No widgets, graphics backend or document ownership here.
// The tool owns its behavior; the viewport merely hosts its description.
class ToolOptions {
public:
    virtual ~ToolOptions() = default;
    [[nodiscard]] virtual std::string_view title() const = 0;
    [[nodiscard]] virtual bool options_available() const = 0;
    virtual void describe_options(vng::editor::Inspector&) = 0;
};
}
