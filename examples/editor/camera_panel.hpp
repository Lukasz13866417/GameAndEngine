#pragma once

#include "project.hpp"
#include <optional>
#include <string>
#include <vng/ui/ui.hpp>

namespace editor_example {
// Looking through a scene camera is private viewport state: Inspect follows
// the camera's evaluated pose read-only, Enter starts from it and lets the
// editor view roam until "Save this camera" authors it back. Neither changes
// the document by itself; Back restores the view from before the visit.
struct CameraVisit {
    vng::u32 camera{};
    bool editable{};
    CameraPose previous{};
    friend bool operator==(const CameraVisit&, const CameraVisit&) = default;
};

// UI-only camera actions drawn in the viewport next to the selected camera's
// glyph, like a gizmo. The host owns the visit state, the editing session and
// status messages; poll clicked() after Screen::update() like any other
// retained control.
class CameraPanel final {
public:
    explicit CameraPanel(vng::ui::Container host) : host_(std::move(host)) {
        host_.gap(6);
        title_ = host_.label("CAMERA / simulation view").height(24);
        auto look = host_.row().height(36).padding(0).gap(6);
        enter_ = look.button("Enter").width(96);
        inspect_ = look.button("Inspect").width(96);
        back_ = look.button("Back").width(96);
        save_ = host_.button("Save this camera").height(36);
        activate_ = host_.button("Set active here").height(36);
        hint_ = host_.label("").height(44);
        host_.visible(false);
    }
    // camera: the selected camera instance, if any. can_author: authored edits
    // are possible at the current playhead (a selected, paused keyframe).
    void sync(const SceneInstance* camera, const std::optional<CameraVisit>& visit, bool active_now, bool can_author,
              bool page_visible = true) {
        shown_ = page_visible && (camera != nullptr || visit.has_value());
        host_.visible(shown_);
        if (!shown_) return;
        const bool visiting = visit.has_value();
        const bool editing = visiting && visit->editable;
        const bool same = visiting && camera && visit->camera == camera->id;
        title_.text(camera ? "CAMERA / " + camera->name : "CAMERA / looking through another camera");
        enter_.visible(camera != nullptr).enabled(camera && !(same && editing));
        inspect_.visible(camera != nullptr).enabled(camera && !(same && !editing));
        back_.enabled(visiting);
        save_.visible(editing).enabled(editing && can_author);
        activate_.visible(camera != nullptr).enabled(camera && can_author && !active_now);
        hint_.text(editing ? "Free view; Save writes it here."
                   : visiting ? "Read-only; Back restores the view."
                   : active_now ? "Simulation camera at this time."
                   : "Not active; Set active keys it.");
    }
    [[nodiscard]] bool shown() const noexcept { return shown_; }
    // Viewport overlay placement; the host decides where the glyph is on screen.
    void place(vng::ui::Rect at) { host_.position({at.x, at.y}).width(at.width).height(at.height); }
    void enabled(bool value) { host_.enabled(value); }
    [[nodiscard]] bool contains(vng::Vec2 point) const { return shown_ && host_.bounds().contains(point); }
    [[nodiscard]] bool enter_clicked() { return enter_.clicked(); }
    [[nodiscard]] bool inspect_clicked() { return inspect_.clicked(); }
    [[nodiscard]] bool back_clicked() { return back_.clicked(); }
    [[nodiscard]] bool save_clicked() { return save_.clicked(); }
    [[nodiscard]] bool activate_clicked() { return activate_.clicked(); }

private:
    vng::ui::Container host_;
    vng::ui::Label title_, hint_;
    vng::ui::Button enter_, inspect_, back_, save_, activate_;
    bool shown_{};
};
} // namespace editor_example
