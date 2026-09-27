#pragma once
#include "editing_session.hpp"

namespace editor_example {
struct RegionEdit {
    enum class Kind { create, erase, replace, points, gesture };
    Kind kind{};
    vng::u32 object{};
    vng::u64 source_revision{};
    RegionShape shape{};
    vng::Vec3 position{};
    vng::f32 radius{};
    std::optional<Region> region{};
    std::vector<vng::u32> vertices{};
    std::vector<RegionPointEdit> points{};
    bool began{}, changed{}, finished{}, cancelled{};
    // Local presentation consequences, applied only after the parent accepts.
    std::optional<std::vector<vng::u32>> select_vertices{};
    std::string success{};
    std::optional<vng::u64> transaction{};
};
struct RegionEditResult {
    bool changed{};
    std::optional<vng::u32> created;
    std::optional<vng::u64> transaction{};
};

// Parent-side domain execution, not a callback held by the UI component.
[[nodiscard]] inline vng::content::Result<RegionEditResult> apply_region_edit(
    EditingSession& editing,const RegionEdit& edit) {
    using Kind=RegionEdit::Kind;
    const auto invalid=[](std::string message)->vng::content::Result<RegionEditResult> {
        vng::content::Diagnostic error;error.message=std::move(message);return std::unexpected(std::move(error));
    };
    if(!edit.cancelled && editing.state().document.revision!=edit.source_revision) {
        // A queued sample can become stale while still owning its capture.
        // Resolve that capture before the child discards its local gesture;
        // never roll back a replacement owner's transaction.
        if(edit.kind==Kind::gesture && edit.transaction &&
            editing.active_transaction()==edit.transaction && editing.active(EditGesture::region) &&
            editing.active_object()==edit.object)
            if(auto cancelled=editing.cancel();!cancelled)return std::unexpected(cancelled.error());
        return invalid("Stale region edit: source revision changed");
    }
    if(edit.kind==Kind::create) {
        auto result=editing.add_region(edit.shape,edit.position,edit.radius);
        if(!result)return std::unexpected(result.error());
        return RegionEditResult{true,*result};
    }
    if(edit.kind==Kind::erase) {
        auto result=editing.erase_region(edit.object);
        if(!result)return std::unexpected(result.error());
        return RegionEditResult{*result,{}};
    }
    const bool standalone=edit.kind!=Kind::gesture;
    if(standalone||edit.began) {
        auto begun=edit.kind==Kind::replace ? editing.begin_region(edit.object)
                                           : editing.begin_region_points(edit.object,edit.vertices);
        if(!begun)return std::unexpected(begun.error());
    }
    if(!editing.active(EditGesture::region)||editing.active_object()!=edit.object ||
        (!standalone&&!edit.began&&(!edit.transaction||editing.active_transaction()!=edit.transaction)))
        return invalid("Region edit no longer owns the active transaction");
    if(edit.cancelled) {
        auto result=editing.cancel();if(!result)return std::unexpected(result.error());
        return RegionEditResult{*result,{}};
    }
    bool changed{};
    if(standalone||edit.changed) {
        if(edit.kind==Kind::replace&&!edit.region) {
            (void)editing.cancel();return invalid("Region replacement has no boundary");
        }
        auto result=edit.kind==Kind::replace ? editing.region(*edit.region) : editing.region_points(edit.points);
        if(!result){(void)editing.cancel();return std::unexpected(result.error());}
        changed=*result;
    }
    if(standalone||edit.finished) {
        auto result=editing.commit();if(!result)return std::unexpected(result.error());
        changed|=*result;
    }
    return RegionEditResult{changed,{},editing.active_transaction()};
}
}
