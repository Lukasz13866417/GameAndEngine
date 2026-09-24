#include "position_edits.hpp"
#include "animation.hpp"
#include "instance_property_edits.hpp"

namespace editor_example {
namespace {
detail::InstancePropertyEdits<PositionSnapshot, PositionEdit,
    &PositionSnapshot::base_position, &PositionEdit::position,
    &InstanceTransform::position> implementation{
        {'V', 'N', 'G', 'P', 'O', 'S', 0, 1}, "position", "Position", scene_coordinate_limit};
}
vng::content::Result<PositionSnapshot> capture_position(const State& state, vng::u32 object) {
    return implementation.capture(state, object);
}
vng::content::Result<void> restore_position(State& state, const PositionSnapshot& value) {
    return implementation.restore(state, value);
}
vng::content::Result<bool> apply_position_value(State& state, vng::u32 object, vng::Vec3 value) {
    return implementation.apply_value(state, object, value);
}
vng::content::Result<bool> apply_placed_position(State& state, vng::u32 object, vng::Vec3 placed) {
    const auto* instance = find_instance(state, object);
    return implementation.apply_value(state, object,
        instance ? stored_position(state, *instance, placed, state.viewport.time) : placed);
}
vng::content::Result<PositionEdit> position_edit(vng::u64 base, const State& state, vng::u32 object) {
    return implementation.make_edit(base, state, object);
}
vng::content::Result<void> apply_position_edit(State& state, const PositionEdit& edit) {
    return implementation.apply_edit(state, edit);
}
vng::content::Result<std::string> encode_position_edit(const PositionEdit& edit) {
    return implementation.encode(edit);
}
vng::content::Result<PositionEdit> decode_position_edit(std::string_view bytes) {
    return implementation.decode(bytes);
}
} // namespace editor_example
