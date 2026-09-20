#include "scale_edits.hpp"
#include "instance_property_edits.hpp"

namespace editor_example {
namespace {
detail::InstancePropertyEdits<ScaleSnapshot, ScaleEdit,
    &ScaleSnapshot::base_scale, &ScaleEdit::scale, &InstanceTransform::scale>
    implementation{{'V','N','G','S','C','L',0,1}, "scale", "Scale", max_instance_scale, min_instance_scale};
struct AxisSnapshot { vng::u32 object; vng::Vec3 value; std::optional<vng::timeline::Track> track; };
struct AxisEdit { vng::u64 base_revision,revision; AxisSnapshot scale; };
detail::InstancePropertyEdits<AxisSnapshot, AxisEdit,
    &AxisSnapshot::value, &AxisEdit::scale, &InstanceTransform::axis_scale>
    axes{{'V','N','G','A','X','S',0,1}, "axis_scale", "Axis scale", max_axis_scale, min_axis_scale};
}
vng::content::Result<ScaleSnapshot> capture_scale(const State& s, vng::u32 id) { return implementation.capture(s,id); }
vng::content::Result<void> restore_scale(State& s, const ScaleSnapshot& v) { return implementation.restore(s,v); }
vng::content::Result<bool> apply_scale_value(State& s, vng::u32 id, vng::f32 v) { return implementation.apply_value(s,id,v); }
vng::content::Result<bool> apply_axis_scale_value(State& s, vng::u32 id, vng::Vec3 v) { return axes.apply_value(s,id,v); }
vng::content::Result<ScaleEdit> scale_edit(vng::u64 base, const State& s, vng::u32 id) { return implementation.make_edit(base,s,id); }
vng::content::Result<void> apply_scale_edit(State& s, const ScaleEdit& v) { return implementation.apply_edit(s,v); }
vng::content::Result<std::string> encode_scale_edit(const ScaleEdit& v) { return implementation.encode(v); }
vng::content::Result<ScaleEdit> decode_scale_edit(std::string_view v) { return implementation.decode(v); }
}
