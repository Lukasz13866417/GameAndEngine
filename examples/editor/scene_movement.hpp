#pragma once
#include "move_gizmo.hpp"
#include "instance_movement.hpp"
#include "component_dispatch.hpp"
#include <algorithm>

namespace editor_example {
// Owns the scene's movement presentation. Known instance transforms use a typed
// binding; genuinely runtime-defined inspector tools retain their native event
// protocol. The two paths do not interpret each other's edit payloads.
class SceneMovement final {
public:
    struct Instance {
        InstanceMovement::Target target;
        std::span<const vng::editor::TranslationAxis> axes{};
    };
    struct Native { const vng::editor::Schema& schema; };
    struct Inactive {};
    struct Reply {
        bool began{}, finished{}, cancelled{};
        std::optional<InstanceMovement::Edit> edit{};
        std::optional<vng::editor::Event> native{};
    };
    [[nodiscard]] const TranslationTool& tool() const { return mode_==Mode::instance?instance_.tool():native_; }
    [[nodiscard]] bool dragging() const { return tool().dragging(); }
    [[nodiscard]] bool visible() const { return tool().visible(); }
    [[nodiscard]] bool handledPointer() const { return handled_; }
    [[nodiscard]] auto selected_axis() const { return tool().selected_axis(); }
    [[nodiscard]] auto handle(std::string_view label,bool reverse=false) const { return tool().handle(label,reverse); }
    void append(vng::ui::DrawList& list,const vng::text::Font& font={}) const { tool().append(list,font); }
    void cancel() { instance_.cancel();native_.cancel();mode_=Mode::inactive; }
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="scene_movement",.role="instance movement and native inspector movement host",
            .situation=mode_==Mode::instance?"instance":mode_==Mode::native?"native":"inactive",
            .owned={{"interacting",debug_bool(dragging())},{"handled input",debug_bool(handled_)}},
            .children={instance_.debug_report()}};
    }
private:
    friend struct Dispatcher;
    enum class Mode { inactive, instance, native };
    using Gizmo=MoveGizmo<InstanceMovement>;
    Reply handle(const Instance& situation,const MoveGizmoContext& supplied) {
        if(mode_==Mode::native&&native_.dragging()) {
            cancel();handled_=true;
            return {.finished=true,.cancelled=true};
        }
        if(mode_!=Mode::instance){native_.cancel();mode_=Mode::instance;}
        auto context=supplied;
        if(!instance_.dragging()) {
            origin_=situation.target;stamp_=context.stamp;
            if(!std::ranges::equal(axes_,situation.axes))axes_.assign(situation.axes.begin(),situation.axes.end());
        } else if(origin_.object!=situation.target.object || stamp_.generation!=context.stamp.generation) {
            // A new target/worker cannot inherit a captured gesture. Notify the
            // authoring owner before accepting input for the replacement.
            auto result=instance_.cancel();
            handled_=true;
            return {.finished=result.finished,.cancelled=result.cancelled};
        }
        // Our own position previews advance the document revision. They do not
        // move the gesture baseline or require a fresh worker inspector.
        context.stamp=stamp_;context.axes=axes_;
        auto result=instance_.update(origin_,context);
        handled_=instance_.handledPointer();
        return {.began=result.began,.finished=result.finished,.cancelled=result.cancelled,.edit=std::move(result.edit)};
    }
    Reply handle(const Native& situation,const MoveGizmoContext& context) {
        if(mode_==Mode::instance&&instance_.dragging()) {
            cancel();handled_=true;
            return {.finished=true,.cancelled=true};
        }
        if(mode_!=Mode::native){instance_.cancel();mode_=Mode::native;}
        const bool was=native_.dragging();
        auto result=native_.update(situation.schema,context.camera,context.viewport,
            context.can_begin||was?context.input:std::span<const vng::input::Event>{},
            context.can_begin||was?context.raw:std::span<const vng::input::Event>{},true,
            context.world_axes,context.arrow_step,context.segment);
        handled_=native_.handledPointer();
        return {.began=!was&&(native_.dragging()||result.has_value()),
            .finished=(was&&!native_.dragging())||result.has_value(),
            .cancelled=was&&!native_.dragging()&&!result,.native=std::move(result)};
    }
    Reply handle(const Inactive&,const MoveGizmoContext&) {
        const bool was=dragging();
        if(mode_!=Mode::inactive)cancel();
        handled_=was;
        return {.finished=was,.cancelled=was};
    }
    Gizmo instance_;
    TranslationTool native_;
    InstanceMovement::Target origin_{};
    vng::editor::Stamp stamp_{};
    std::vector<vng::editor::TranslationAxis> axes_;
    Mode mode_{Mode::inactive};
    bool handled_{};
};
}
