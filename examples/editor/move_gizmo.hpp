#pragma once

#include "component_debug.hpp"
#include "translation_tool.hpp"

namespace editor_example {
struct MoveGizmoContext {
    vng::editor::Stamp stamp;
    const vng::gfx::CameraSnapshot& camera;
    vng::ui::Rect viewport;
    std::span<const vng::input::Event> input{}, raw{};
    bool can_begin{true};
    bool world_axes{true};
    float arrow_step{1};
    std::span<const vng::editor::TranslationAxis> axes{};
    std::optional<TranslationSegment> segment{};
};

// A complete reusable axis gizmo. Movement is a stateless compile-time bridge;
// only input/visual state is retained here, never Target or its edit authority.
// The owner freezes its small target baseline/stamp for the duration of a drag,
// applies returned proposals and owns begin/commit/cancel of the transaction.
// It calls update/append only for the selected gizmo and cancel when retiring it.
// Movement supplies Target, Edit, world_position(const Target&) -> Vec3 and
// move_to(const Target&, Vec3) -> Edit. No base class or concept is required.
template<class Movement>
class MoveGizmo final {
public:
    using Target = typename Movement::Target;
    using Edit = typename Movement::Edit;
    struct Reply {
        bool began{}, changed{}, finished{}, cancelled{};
        std::optional<Edit> edit{};
    };

    [[nodiscard]] Reply update(const Target& target,const MoveGizmoContext& context) {
        const bool was=tool_.dragging();
        if(was&&stamp_!=context.stamp)return cancel();
        stamp_=context.stamp;can_begin_=context.can_begin;
        const auto origin=Movement::world_position(target);
        if(!was)last_position_=origin;
        const auto result=tool_.update(TranslationTarget{context.stamp,"move",origin,context.axes},
            context.camera,context.viewport,
            context.can_begin||was?context.input:std::span<const vng::input::Event>{},
            context.can_begin||was?context.raw:std::span<const vng::input::Event>{},
            true,context.world_axes,context.arrow_step,context.segment);
        handled_=tool_.handledPointer();
        Reply reply{.began=!was&&(tool_.dragging()||result.has_value()),
            .finished=(was&&!tool_.dragging())||result.has_value(),
            .cancelled=was&&!tool_.dragging()&&!result};
        const auto position=result?result:tool_.preview_position();
        if(!reply.cancelled&&position) {
            reply.changed=position!=last_position_;
            if(reply.changed)reply.edit=Movement::move_to(target,*position);
            last_position_=position;
        }
        return reply;
    }
    // Cleanup needs no camera/input/target. The host can use the returned
    // cancellation to roll back a transaction; repeated cleanup is a no-op.
    Reply cancel() {
        const bool was=tool_.dragging();
        tool_.cancel();last_position_.reset();can_begin_=false;handled_=was;
        return {.finished=was,.cancelled=was};
    }

    [[nodiscard]] const TranslationTool& tool() const { return tool_; }
    void clear_selection() { tool_.clear_selection(); }
    [[nodiscard]] bool dragging() const { return tool_.dragging(); }
    [[nodiscard]] bool handledPointer() const { return handled_; }
    [[nodiscard]] bool visible() const { return tool_.visible(); }
    [[nodiscard]] auto selected_axis() const { return tool_.selected_axis(); }
    [[nodiscard]] auto handle(std::string_view label, bool reverse=false) const { return tool_.handle(label,reverse); }
    void append(vng::ui::DrawList& list,const vng::text::Font& font={},int axis=-1) const { tool_.append(list,font,axis); }

    [[nodiscard]] DebugReport debug_report() const {
        return {.name="move",.role="typed target movement; proposes edits without owning the target",
            .situation=dragging()?"interacting":"idle",
            .received={{"last object",std::to_string(stamp_.object)},{"last generation",std::to_string(stamp_.generation)},
                {"new gesture allowed",debug_bool(can_begin_)}},
            .owned={{"visible",debug_bool(visible())},{"interacting",debug_bool(dragging())},
                {"selected axis",selected_axis()?std::to_string(*selected_axis()):"none"}}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }

private:
    TranslationTool tool_;
    std::optional<vng::Vec3> last_position_;
    vng::editor::Stamp stamp_{};
    bool handled_{},can_begin_{};
};
} // namespace editor_example
