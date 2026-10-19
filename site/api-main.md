# vgl API reference {#mainpage}

The public interface of [vgl](../index.html), generated from its headers. vgl's stable
ABI is the C API; the C++ wrapper (the `.hpp` headers, namespace `vgl`) is header-only on top
of it.

| Header | Contents |
|---|---|
| `vgl/vgl.h` | Includes every core header |
| `vgl/base.h` | Results, version, vectors and matrices, export macros |
| `vgl/foundation.h` | Allocator, logging, I/O and task interfaces |
| `vgl/geodesy.h` | Ellipsoids, geodetic and ECEF coordinates, ENU frames |
| `vgl/context.h` | The context: packs, views, updates, queries, statistics, attribution |
| `vgl/render.h` | The render interface: frames, resources and draw lists |
| `vgl/defaults.h` | Thread pool, file reader, HTTP reader and disk cache |
| `vgl/gl.h` | OpenGL / OpenGL ES backend |
| `vgl/d3d11.h` | Direct3D 11 backend |
| `vgl/ogre.hpp` | Ogre 14 backend (C++ only) |

The conventions behind the API are in the
[C API conventions](https://github.com/3dgep/vgl/blob/main/docs/c-api.md) and the
[render interface specification](https://github.com/3dgep/vgl/blob/main/docs/render-interface.md);
the [integrator's guide](https://github.com/3dgep/vgl/wiki) explains how to use them.
