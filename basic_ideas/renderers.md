# Possible way to render stuff
> Renderer specialized for given things
If there was one rendering API backend (either opengl, metal or vulkan), it would be easier. Just make a `Renderer<ThingToBeRendered>`. But the backend is also needed.
Because sometimes you need more performance and need to leverage backend-specific stuff.
So I'd make a class `Renderer<backend>`, and then make a class `OpenGLRenderer<ThingToBeRendered>` derived from `Renderer<OpenGLContext>`.
The `ThingToBeRendered` type needs to expose some info needed to render. If sometimes one renderer will render different instances, we can simply add compile-time requirements or variadic template args.
The renderer can own the mesh & material (e.g. when we want to render 100 teapots that have the same mesh and surface, and), but doesn't have to.