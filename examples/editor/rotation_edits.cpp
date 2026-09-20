#include "rotation_edits.hpp"
#include "instance_property_edits.hpp"

namespace editor_example {
namespace {
detail::InstancePropertyEdits<RotationSnapshot, RotationEdit,
    &RotationSnapshot::base_rotation, &RotationEdit::rotation,
    &InstanceTransform::rotation> implementation{
        {'V', 'N', 'G', 'R', 'O', 'T', 0, 1}, "rotation", "Rotation", 360.F};
}
vng::content::Result<RotationSnapshot> capture_rotation(const State& state, vng::u32 object) {
    return implementation.capture(state, object);
}
vng::content::Result<void> restore_rotation(State& state, const RotationSnapshot& value) {
    return implementation.restore(state, value);
}
vng::content::Result<bool> apply_rotation_value(State& state, vng::u32 object, vng::Vec3 value) {
    return implementation.apply_value(state, object, value);
}
vng::content::Result<RotationEdit> rotation_edit(vng::u64 base, const State& state, vng::u32 object) {
    return implementation.make_edit(base, state, object);
}
vng::content::Result<void> apply_rotation_edit(State& state, const RotationEdit& edit) {
    return implementation.apply_edit(state, edit);
}
vng::content::Result<std::string> encode_rotation_edit(const RotationEdit& edit) {
    return implementation.encode(edit);
}
vng::content::Result<RotationEdit> decode_rotation_edit(std::string_view bytes) {
    return implementation.decode(bytes);
}
} // namespace editor_example
