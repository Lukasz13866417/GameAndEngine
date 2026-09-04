# Possible way to make backends swappable
> Window backends
```c++
auto windowContext = make_sdl_context(arg1, arg2, arg3);
auto window = make_window(windowContext); // detects type of windowContext and returns SDLWindow object, which is Window<SDLContext>, for example (so a specialization).
// SDLWindow can also be a subclass of Window<SDLWindow> (so CRTP)
```