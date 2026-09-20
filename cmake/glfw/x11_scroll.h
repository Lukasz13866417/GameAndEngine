/* VNG addition to GLFW 3.4. Included inside x11_window.c so scroll is decoded
 * in GLFW's existing ordered event pump, on the owning event thread. */
#include "scroll_axes.h"

static int vngTraceScroll(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char* value = getenv("VNG_TRACE_SCROLL");
        enabled = value && strcmp(value, "1") == 0;
    }
    return enabled;
}

static int vngHasScrollSample(const VngScrollAxis* axis, const XIValuatorState* values)
{
    return axis->number >= 0 && axis->number / 8 < values->mask_len &&
           XIMaskIsSet(values->mask, axis->number);
}

typedef struct VngScrollDevice {
    int id;
    unsigned long baselineSerial;
    VngScrollAxes axes;
    struct VngScrollDevice* next;
} VngScrollDevice;

static void vngClearScroll(_GLFWwindow* window)
{
    VngScrollDevice* device = window->x11.scroll;
    while (device)
    {
        VngScrollDevice* next = device->next;
        _glfw_free(device);
        device = next;
    }
    window->x11.scroll = NULL;
}

static VngScrollDevice* vngSetScrollDevice(_GLFWwindow* window, int id,
                                        int count, XIAnyClassInfo** classes)
{
    VngScrollDevice* device;
    for (device = window->x11.scroll; device; device = device->next)
        if (device->id == id)
            break;
    if (!device)
    {
        device = _glfw_calloc(1, sizeof(VngScrollDevice));
        if (!device)
            return NULL;
        device->id = id;
        device->next = window->x11.scroll;
        window->x11.scroll = device;
    }
    device->axes = vngScrollAxes(count, classes);
    return device;
}

static void vngRefreshScroll(_GLFWwindow* window)
{
    int count, i;
    unsigned long serial;
    XIDeviceInfo* devices;
    if (!window->x11.smoothScroll)
        return;
    devices = XIQueryDevice(_glfw.x11.display, XIAllMasterDevices, &count);
    if (!devices)
        return;
    serial = LastKnownRequestProcessed(_glfw.x11.display);
    vngClearScroll(window);
    for (i = 0; i < count; ++i)
    {
        VngScrollDevice* device = vngSetScrollDevice(window, devices[i].deviceid,
                                        devices[i].num_classes, devices[i].classes);
        if (device)
        {
            device->baselineSerial = serial;
            if (vngTraceScroll()) fprintf(stderr,
                "[scroll/x11] device=%d vertical=%d previous=%.9g increment=%.9g horizontal=%d\n",
                device->id, device->axes.vertical.number, device->axes.vertical.previous,
                device->axes.vertical.increment, device->axes.horizontal.number);
        }
    }
    XIFreeDeviceInfo(devices);
    if (vngTraceScroll()) fprintf(stderr, "[scroll/x11] refresh window=%lu baseline_serial=%lu\n",
                                 window->x11.handle, serial);
}

static void vngInitScroll(_GLFWwindow* window)
{
    unsigned char bits[XIMaskLen(XI_Enter)] = {0};
    XIEventMask mask;
    if (vngTraceScroll()) fprintf(stderr, "[scroll/x11] initialize window=%lu XI=%d.%d available=%d\n",
        window->x11.handle, _glfw.x11.xi.major, _glfw.x11.xi.minor, _glfw.x11.xi.available);
    if (!_glfw.x11.xi.available || _glfw.x11.xi.major < 2 ||
        (_glfw.x11.xi.major == 2 && _glfw.x11.xi.minor < 1) ||
        !_glfw.x11.xi.QueryDevice || !_glfw.x11.xi.FreeDeviceInfo)
        return;
    XISetMask(bits, XI_Motion);
    XISetMask(bits, XI_ButtonPress);
    XISetMask(bits, XI_ButtonRelease);
    XISetMask(bits, XI_DeviceChanged);
    XISetMask(bits, XI_Enter);
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;
    _glfwGrabErrorHandlerX11();
    XISelectEvents(_glfw.x11.display, window->x11.handle, &mask, 1);
    _glfwReleaseErrorHandlerX11();
    if (_glfw.x11.errorCode)
        return; /* Older servers retain GLFW's ordinary wheel path. */
    window->x11.smoothScroll = GLFW_TRUE;
    vngRefreshScroll(window);
}

static void vngScrollEvent(XGenericEventCookie* cookie, void (*processCore)(XEvent*))
{
    _GLFWwindow* window;
    XIDeviceEvent* event;
    VngScrollDevice* device;
    if (cookie->evtype == XI_DeviceChanged)
    {
        XIDeviceChangedEvent* changed = cookie->data;
        /* Classes include event-time valuator baselines when a master switches
         * between physical devices. Never reuse the previous mouse's axes. */
        for (window = _glfw.windowListHead; window; window = window->next)
            if (window->x11.smoothScroll)
            {
                VngScrollDevice* device = vngSetScrollDevice(window, changed->deviceid,
                                             changed->num_classes, changed->classes);
                if (device)
                {
                    device->baselineSerial = changed->serial;
                    if (vngTraceScroll()) fprintf(stderr,
                        "[scroll/x11] device-changed device=%d source=%d serial=%lu vertical=%d previous=%.9g increment=%.9g\n",
                        changed->deviceid, changed->sourceid, changed->serial, device->axes.vertical.number,
                        device->axes.vertical.previous, device->axes.vertical.increment);
                }
            }
        return;
    }
    if (cookie->evtype == XI_Enter)
    {
        XIEnterEvent* entered = cookie->data;
        if (vngTraceScroll()) fprintf(stderr, "[scroll/x11] enter window=%lu mode=%d detail=%d time=%lu\n",
            entered->event, entered->mode, entered->detail, entered->time);
        if (XFindContext(_glfw.x11.display, entered->event, _glfw.x11.context,
                         (XPointer*) &window) == 0)
            vngRefreshScroll(window); /* Exclude scrolling outside this window. */
        return;
    }
    if (cookie->evtype != XI_Motion && cookie->evtype != XI_ButtonPress &&
        cookie->evtype != XI_ButtonRelease)
        return;
    event = cookie->data;
    if (XFindContext(_glfw.x11.display, event->event, _glfw.x11.context,
                     (XPointer*) &window) != 0 || !window->x11.smoothScroll)
        return;
    /* XI2 replaces core delivery for the selected pointer events, not just
     * scrolling. Reuse GLFW's existing motion/button dispatch (including
     * disabled/raw cursor handling and extra button mapping) exactly once. */
    if (cookie->evtype == XI_Motion)
    {
        XEvent core = {0};
        core.xmotion.type = MotionNotify;
        core.xmotion.serial = event->serial;
        core.xmotion.send_event = event->send_event;
        core.xmotion.display = event->display;
        core.xmotion.window = event->event;
        core.xmotion.root = event->root;
        core.xmotion.subwindow = event->child;
        core.xmotion.time = event->time;
        core.xmotion.x = (int) event->event_x;
        core.xmotion.y = (int) event->event_y;
        core.xmotion.x_root = (int) event->root_x;
        core.xmotion.y_root = (int) event->root_y;
        core.xmotion.state = event->mods.effective;
        core.xmotion.is_hint = NotifyNormal;
        core.xmotion.same_screen = True;
        processCore(&core);
    }
    else if (event->detail < 4 || event->detail > 7)
    {
        XEvent core = {0};
        core.xbutton.type = cookie->evtype == XI_ButtonPress ? ButtonPress : ButtonRelease;
        core.xbutton.serial = event->serial;
        core.xbutton.send_event = event->send_event;
        core.xbutton.display = event->display;
        core.xbutton.window = event->event;
        core.xbutton.root = event->root;
        core.xbutton.subwindow = event->child;
        core.xbutton.time = event->time;
        core.xbutton.x = (int) event->event_x;
        core.xbutton.y = (int) event->event_y;
        core.xbutton.x_root = (int) event->root_x;
        core.xbutton.y_root = (int) event->root_y;
        core.xbutton.state = event->mods.effective;
        core.xbutton.button = event->detail;
        core.xbutton.same_screen = True;
        processCore(&core);
        return;
    }
    for (device = window->x11.scroll; device; device = device->next)
        if (device->id == event->deviceid)
            break;
    if (cookie->evtype == XI_Motion && device)
    {
        const int tracing = vngTraceScroll() &&
            (vngHasScrollSample(&device->axes.horizontal, &event->valuators) ||
             vngHasScrollSample(&device->axes.vertical, &event->valuators));
        const double before = device->axes.vertical.previous;
        /* Re-entry queries a current server snapshot. Older events may already
         * be queued: comparing them with that newer baseline would briefly
         * scroll backwards. Resume from events at/after the snapshot instead. */
        if (event->serial < device->baselineSerial)
        {
            if (tracing) fprintf(stderr, "[scroll/x11] stale-motion device=%d time=%lu serial=%lu baseline_serial=%lu\n",
                                 event->deviceid, event->time, event->serial, device->baselineSerial);
            return;
        }
        const double x = vngScrollDelta(&device->axes.horizontal, &event->valuators);
        const double y = vngScrollDelta(&device->axes.vertical, &event->valuators);
        if (tracing) fprintf(stderr,
            "[scroll/x11] motion device=%d source=%d time=%lu serial=%lu previous=%.9g current=%.9g dx=%.9g dy=%.9g\n",
            event->deviceid, event->sourceid, event->time, event->serial, before,
            device->axes.vertical.previous, x, y);
        if (x != 0 || y != 0)
        {
            _glfwInputScroll(window, x, y);
        }
    }
    else if (cookie->evtype == XI_ButtonPress)
    {
        const int deliver = !device || vngScrollButton(&device->axes, event->detail, event->flags);
        if (vngTraceScroll()) fprintf(stderr,
            "[scroll/x11] button device=%d source=%d time=%lu button=%d flags=%d deliver=%d\n",
            event->deviceid, event->sourceid, event->time, event->detail, event->flags, deliver);
        if (!deliver) return;
        if (event->detail >= 4 && event->detail <= 7)
        {
            if (window->cursorMode != GLFW_CURSOR_DISABLED)
                _glfwInputCursorPos(window, event->event_x, event->event_y);
            if (event->detail == 4) _glfwInputScroll(window, 0, 1);
            if (event->detail == 5) _glfwInputScroll(window, 0, -1);
            if (event->detail == 6) _glfwInputScroll(window, 1, 0);
            if (event->detail == 7) _glfwInputScroll(window, -1, 0);
        }
    }
}
