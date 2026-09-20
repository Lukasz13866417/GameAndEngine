/* VNG's private GLFW 3.4 XInput 2.1 adapter. No engine/public API dependency. */
#ifndef VNG_GLFW_SCROLL_AXES_H
#define VNG_GLFW_SCROLL_AXES_H

#include <X11/extensions/XInput2.h>
#include <math.h>

typedef struct VngScrollAxis {
    int number;
    int preferred;
    double increment;
    double previous;
} VngScrollAxis;

typedef struct VngScrollAxes {
    VngScrollAxis horizontal;
    VngScrollAxis vertical;
} VngScrollAxes;

static VngScrollAxes vngScrollAxes(int count, XIAnyClassInfo** classes)
{
    VngScrollAxes axes = {{-1, 0, 0, 0}, {-1, 0, 0, 0}};
    int i, j;
    for (i = 0; i < count; ++i)
    {
        const XIScrollClassInfo* scroll;
        VngScrollAxis* axis;
        if (classes[i]->type != XIScrollClass)
            continue;
        scroll = (const XIScrollClassInfo*) classes[i];
        if (!isfinite(scroll->increment) || scroll->increment == 0 || scroll->number < 0)
            continue;
        if (scroll->scroll_type == XIScrollTypeVertical)
            axis = &axes.vertical;
        else if (scroll->scroll_type == XIScrollTypeHorizontal)
            axis = &axes.horizontal;
        else
            continue;
        if (axis->number >= 0 && (axis->preferred || !(scroll->flags & XIScrollFlagPreferred)))
            continue;
        for (j = 0; j < count; ++j)
        {
            const XIValuatorClassInfo* value;
            if (classes[j]->type != XIValuatorClass)
                continue;
            value = (const XIValuatorClassInfo*) classes[j];
            if (value->number != scroll->number || !isfinite(value->value))
                continue;
            axis->number = scroll->number;
            axis->preferred = !!(scroll->flags & XIScrollFlagPreferred);
            axis->increment = scroll->increment;
            axis->previous = value->value;
            break;
        }
    }
    return axes;
}

static double vngScrollDelta(VngScrollAxis* axis, const XIValuatorState* values)
{
    int bit, index = 0;
    double value, delta;
    if (axis->number < 0 || axis->number / 8 >= values->mask_len ||
        !XIMaskIsSet(values->mask, axis->number))
        return 0;
    /* Values are densely packed; valuator numbers need not be contiguous. */
    for (bit = 0; bit < axis->number; ++bit)
        if (XIMaskIsSet(values->mask, bit))
            ++index;
    value = values->values[index];
    if (!isfinite(value))
        return 0;
    delta = (axis->previous - value) / axis->increment;
    axis->previous = value;
    return isfinite(delta) ? delta : 0;
}

/* XI2 emits compatibility buttons as well as motion for smooth wheels.
 * Only suppress emulation for an axis we actually understand. Real legacy
 * buttons (including XTEST/remote desktop input) must continue to work. */
static int vngScrollButton(const VngScrollAxes* axes, int button, int flags)
{
    if (!(flags & XIPointerEmulated))
        return 1;
    if (button == 4 || button == 5)
        return axes->vertical.number < 0;
    if (button == 6 || button == 7)
        return axes->horizontal.number < 0;
    return 1;
}

#endif
