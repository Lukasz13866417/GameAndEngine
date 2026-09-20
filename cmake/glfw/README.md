# X11 fractional scrolling

GLFW 3.4's X11 backend turns core buttons 4–7 into whole wheel steps. The engine
already carries floating-point scroll deltas, so smoothing the editor camera
cannot recover the missing input. `smooth-scroll.patch` connects the existing
GLFW event pump to the XInput 2.1 decoder in these private headers.

- Scroll classes describe the valuator number and units per wheel step. Sparse
  valuators are decoded as doubles; even a fraction of a step is delivered to
  the ordinary GLFW callback immediately. No sensitivity multiplier, integer
  rounding, accumulation threshold, extra thread or second event connection.
- Device-class changes replace the axes/baselines; entering a window refreshes
  them so scrolling elsewhere cannot cause a zoom jump on return. Events older
  than that server snapshot are discarded during re-entry to avoid interpreting
  an old sample as a backwards scroll. Normal in-window scrolling has no gate.
- XI2 compatibility buttons are ignored only for supported smooth axes. Real
  legacy/XTEST wheel buttons still work, exactly once. The old core-button path
  remains available when XInput 2.1 cannot be enabled.
- XI2 replaces ordinary pointer delivery too. Motion and non-wheel buttons are
  forwarded through GLFW's existing core dispatch, preserving modifiers, grabs,
  releases outside the window, raw/disabled cursor handling and button mapping.
- Wayland is unchanged. GLFW raw pointer movement and the public `vng::input`
  event API are unchanged. There is no X11 dependency in the editor/navigation
  code or graphics backends.

On Linux, `vng_provide_glfw()` deliberately builds the pinned GLFW 3.4 source
instead of selecting an installed, unpatched GLFW. Other platforms still prefer
installed packages. This is a local, explicitly marked dependency patch, not an
upstream GLFW feature. `ApplyPatch.cmake` applies it once and checks it on each
configure; incompatible local dependency edits produce an error, not an
overwrite. Review/remove the patch when adopting upstream smooth X11 input.
An explicit GLFW target supplied by a parent CMake project is retained, with a
warning that this project cannot patch that dependency.

`vng_scroll_input_tests` tests fractional normalization, sparse masks, both axes,
device changes and emulation filtering without a display. `vng_x11_scroll_tests`
uses XTEST inside Xvfb or Xephyr to verify the real event subscription and legacy
delivery, modifier clicks and captured drags without moving the user's desktop
pointer. Existing window, navigation,
zoom-motion and full editor walkthrough tests cover fractional events through
the neutral queue, UI target, worker and presented camera.

A physical wheel still needs to emit high-resolution data. Unlocking mechanical
detents alone does not guarantee that the device/driver reports smaller deltas.
