# vgl C API Conventions

| | |
|---|---|
| **Status** | Draft 0.1 |
| **Date** | 13 October 2026 |
| **Author** | Jeremiah van Oosten |
| **Scope** | Rules every public C declaration follows. Defines no library functionality beyond the base header. |

This specification expands section 7.1 of the [design document](design.md). Every function, type and header in the public C API follows it, and code review checks new API against the checklist in section 17. Where this document and the design document disagree, the design document wins until both are corrected.

The key words "must", "must not", "should" and "may" are used in their usual specification sense.

---

## 1. Goals

The C API is the only ABI of the library (NFR-01). The conventions serve four goals, in order of priority:

1. **ABI stability.** A host compiled against an older header keeps working with a newer binary of the same major version, on any compiler and standard library.
2. **Predictability.** Every function behaves the same way with respect to errors, outputs, ownership and threading, so a reader who knows one function knows the shape of all of them.
3. **Ease of use from C.** The minimal integration (design document, section 13.2) should be short and obvious to write in plain C. The pure-C sample is the test of this.
4. **Ease of binding.** The API should map mechanically onto the C++ wrapper and onto other languages through a foreign function interface.

---

## 2. Language and headers

### 2.1 Language level

Public C headers are written in C99 and must also compile without warnings as C++ (C++11 and later). They must not use:

- `inline` or `static` functions. Every operation is an exported function, so bindings never need to reimplement logic.
- `bool` or `_Bool`, bit fields, flexible array members, variable-length arrays or anonymous unions.
- `long` or `unsigned long`, whose size differs between Windows and Linux.
- Compiler-specific extensions, except inside the macros of section 14.

### 2.2 Header layout

| Header | Contents |
|---|---|
| `vgl/vgl.h` | Umbrella header. Includes every core header below. |
| `vgl/base.h` | Export macros, basic types, result codes, version |
| `vgl/foundation.h` | Allocator, logging, task and I/O interfaces |
| `vgl/geodesy.h` | Ellipsoids, coordinate conversions, geoid |
| `vgl/context.h` | Context, layers and packs, update, queries, statistics |
| `vgl/render.h` | Render interface and visible-set description |
| `vgl/defaults.h` | Optional thread pool and filesystem reader |
| `vgl/backend_gl.h` | OpenGL / OpenGL ES backend |
| `vgl/backend_d3d11.h` | Direct3D 11 backend |
| `vgl/backend_ogre.hpp` | Ogre backend (C++ only, because Ogre is a C++ API) |
| `vgl/vgl.hpp`, `vgl/<module>.hpp` | Header-only C++ wrapper (section 16) |

New modules get their own header and are added to `vgl/vgl.h`. Backend and defaults headers are not included by the umbrella header, because they are optional and separately linked.

### 2.3 Header rules

- Include guards use `VGL_<HEADER>_H`, for example `VGL_BASE_H`. `#pragma once` is not used.
- Declarations are wrapped in `extern "C"` when compiled as C++.
- Core headers include only `<stddef.h>`, `<stdint.h>` and other vgl headers. They never include platform headers such as `<windows.h>`.
- Backend headers may include the headers of their graphics API where the types are needed, for example `<d3d11.h>`. The OpenGL backend header includes no OpenGL header: object names are passed as `uint32_t`, which matches `GLuint`.
- Each header must compile on its own, in any include order.

---

## 3. Naming

### 3.1 Identifiers

| Kind | Convention | Example |
|---|---|---|
| Function | `vgl_<object>_<action>` | `vgl_context_create` |
| Opaque object type | `vgl_<object>` | `vgl_context` |
| Structure | `vgl_<name>` | `vgl_context_create_info` |
| Structure field | snake case | `cpu_budget` |
| Enumeration type | `vgl_<name>` | `vgl_log_level` |
| Enumeration value | `VGL_<TYPE>_<VALUE>` | `VGL_LOG_LEVEL_WARNING` |
| Flags type and bits | `vgl_<name>_flags` and `VGL_<NAME>_FLAG_<BIT>` | `vgl_layer_flags`, `VGL_LAYER_FLAG_HIDDEN` |
| Macro or constant | `VGL_<TYPE>_<VALUE>` or `VGL_<NAME>` | `VGL_VERSION_MAJOR` |
| Callback type | `vgl_<name>_fn` | `vgl_log_fn` |
| Extension interface | `vgl_<name>_interface` | `vgl_provider_interface` |
| Creation parameters | `vgl_<object>_create_info` | `vgl_context_create_info` |

All identifiers beginning with `vgl_` or `VGL_` are reserved for the library. Macros that are public only for technical reasons use the `VGL_INTERNAL_` prefix and are not part of the API. Identifiers containing a double underscore are not used, because C++ reserves them.

### 3.2 Function names

A function name is the object, optionally followed by a sub-object or property, followed by the action: `vgl_<object>[_<sub-object or property>]_<action>`. The object comes first so that related functions sort and autocomplete together.

| Action | Meaning | Example |
|---|---|---|
| `create`, `destroy` | Create or destroy an object | `vgl_context_create` |
| `get`, `set` | Read or write a property or a value | `vgl_context_statistics_get` |
| `add`, `remove` | Add an item to, or remove it from, an object | `vgl_context_layer_add` |
| `query` | Compute a result that may depend on loaded data | `vgl_context_height_query` |
| `update`, `draw` | Per-frame operations | `vgl_context_update` |
| `complete` | Finish an asynchronous request (section 12) | `vgl_io_request_complete` |
| `<a>_to_<b>` | Convert between representations | `vgl_ellipsoid_geodetic_to_ecef` |

Functions that need no object use the concept they act on as the object, for example `vgl_version_get` and `vgl_result_name_get`.

### 3.3 Parameter names

- Parameters are snake case.
- Output parameters start with `out_`, and parameters that are both read and written start with `inout_`.
- Element counts end in `_count` and byte sizes in `_size`.
- The object parameter is named after its type without the prefix: `vgl_context* context`.

---

## 4. Basic types and units

### 4.1 Scalars

| Use | Type |
|---|---|
| Integers | Fixed-width types from `<stdint.h>` |
| Sizes of memory blocks | `size_t` |
| File offsets, byte ranges, memory budgets | `uint64_t` |
| Element counts | `uint32_t` |
| Booleans | `vgl_bool` (`uint32_t`), with `VGL_TRUE` (1) and `VGL_FALSE` (0) |
| Positions, distances, angles, times | `double` |
| GPU-facing data (vertices, texture coordinates) | `float`, where documented |
| Text | `char`, holding UTF-8 |

On input, any non-zero `vgl_bool` is true. On output, the library writes exactly `VGL_TRUE` or `VGL_FALSE`.

### 4.2 Vectors and matrices

`vgl/base.h` defines `vgl_dvec2`, `vgl_dvec3` and `vgl_dvec4`, structures of `double` fields named `x`, `y`, `z` and `w`, and `vgl_dmat4`, a structure holding `double m[16]`.

Matrices are stored in column-major order and multiply column vectors: element (row *r*, column *c*) is `m[c * 4 + r]`, and the translation is in `m[12]`, `m[13]` and `m[14]`. This matches the memory layout of GLM and of OpenGL uniforms. A host with row-major, row-vector matrices (common with Direct3D math libraries) can pass its matrices unchanged, because the transposition and the change of multiplication order cancel out.

### 4.3 Units and coordinate systems

- Lengths and heights are in metres, and times and durations in seconds.
- Angles, including latitude and longitude, are in radians. The geodesy module provides conversions from degrees.
- Heights are ellipsoidal heights relative to WGS84 (or the context's ellipsoid), unless the function or field name says otherwise, for example `height_msl`.
- Earth-centred, Earth-fixed (ECEF) coordinates are right-handed, with the origin at the centre of the ellipsoid, +Z through the north pole and +X through latitude 0, longitude 0.

### 4.4 Strings

- All strings are UTF-8 and null-terminated, including file paths and URIs.
- Input strings are copied before the function returns, unless the function documents otherwise.
- Output strings are `const char*` pointers to memory owned by the library. Each function documents how long the pointer stays valid. The host never frees them.

### 4.5 Enumerations

```c
typedef enum vgl_log_level
{
    VGL_LOG_LEVEL_DEFAULT = 0,
    VGL_LOG_LEVEL_ERROR = 1,
    VGL_LOG_LEVEL_WARNING = 2,
    VGL_LOG_LEVEL_INFO = 3,
    VGL_LOG_LEVEL_DEBUG = 4,
    VGL_LOG_LEVEL_MAX_ENUM = 0x7FFFFFFF
} vgl_log_level;
```

- Every value is assigned explicitly. Values are never renumbered or reused, even after they are deprecated.
- The last value is `VGL_<TYPE>_MAX_ENUM = 0x7FFFFFFF`, which keeps the type 32 bits wide on every compiler. It is not a valid argument.
- An enumeration used in a versioned structure has a value 0 that means "default" or "none", so that a zero-initialized structure is valid (section 8).
- Functions reject values they do not know with `VGL_RESULT_INVALID_ARGUMENT`.

### 4.6 Flags

A set of flags is a `uint32_t` typedef named `vgl_<name>_flags`. The individual bits are an enumeration named `vgl_<name>_flag` with values `VGL_<NAME>_FLAG_<BIT>`. The value 0 means "no flags" and must always be a sensible default. Undefined bits must be zero; the library rejects unknown bits with `VGL_RESULT_INVALID_ARGUMENT`.

---

## 5. Objects

### 5.1 Handles

Objects are opaque structures that are declared but never defined in the public headers, and are passed by pointer:

```c
typedef struct vgl_context vgl_context;
```

A null pointer means "no object". Handles are not reference counted, not shared between contexts, and not valid after the object is destroyed. Using a destroyed handle is undefined behaviour. Debug builds try to detect it and log an error.

### 5.2 Creation

```c
vgl_result vgl_context_create(const vgl_context_create_info* info, vgl_context** out_context);
vgl_result vgl_layer_create(vgl_context* context, const vgl_layer_create_info* info, vgl_layer** out_layer);
```

- The parent object, if there is one, is the first parameter. The new handle is returned through the last parameter.
- `info` may be null, which means all defaults. This keeps the minimal integration short.
- On failure, `*out_<object>` is set to null.

### 5.3 Destruction

```c
vgl_result vgl_context_destroy(vgl_context* context);
```

- Destroying a null handle does nothing and returns `VGL_RESULT_SUCCESS`.
- Destruction of a valid handle always succeeds. The result code exists only so that every function has the same shape (design document, section 7.1).
- Objects created from a parent belong to that parent. Destroying the parent also destroys its remaining children and logs a warning for each one, because it usually indicates a leak in the host. Hosts should destroy children first.
- Objects that hold GPU resources document on which thread they must be destroyed (section 11).

---

## 6. Functions

### 6.1 Shape

Every function returns `vgl_result`. Parameters appear in this order: the object, then inputs, then outputs.

```c
vgl_result vgl_context_height_query(vgl_context* context, double latitude, double longitude, double* out_height);
```

### 6.2 Outputs

- Outputs are written through pointer parameters. Output pointers must not be null unless the function documents them as optional.
- On failure, every output that is a pointer or handle is set to null, and other outputs are left unmodified. On success, every output is written.
- An output pointer must not alias an input of the same call.

### 6.3 Variable-length outputs

Functions that return a variable number of elements use a two-call pattern with an in/out count:

```c
vgl_result vgl_context_attribution_get(vgl_context* context, uint32_t* inout_count, const char** out_strings);
```

1. If `out_strings` is null, the function writes the number of available elements to `*inout_count` and returns `VGL_RESULT_SUCCESS`.
2. Otherwise, `*inout_count` holds the capacity of the array. The function writes up to that many elements, sets `*inout_count` to the number written, and returns `VGL_RESULT_INCOMPLETE` if more were available, or `VGL_RESULT_SUCCESS` if not.

Byte buffers use the same pattern with a `size_t* inout_size`.

### 6.4 Views into library memory

For per-frame data, such as the visible tile set, a function may instead return a pointer to an array owned by the library, together with its count. This avoids copies and allocations on the render path. The function documents exactly when the view becomes invalid, typically at the next call to `vgl_context_update` on the same context. The host must not write through such pointers.

### 6.5 Ownership of inputs

The library does not retain pointers passed to it. It copies what it needs, including strings, structures and arrays, before the function returns. There are two exceptions:

- `user_data` pointers are retained and passed back to callbacks, but never dereferenced.
- A function that takes ownership of a large host buffer, to avoid a copy, says so and takes a release callback that the library calls when it no longer needs the buffer.

### 6.6 Argument validation

Every function checks, in all build types:

- that required pointers and handles are not null,
- the `struct_size` field of versioned structures (section 8),
- that enumeration values and flags are known, and that values are within their documented ranges.

A failed check returns `VGL_RESULT_INVALID_ARGUMENT` (or `VGL_RESULT_NOT_SUPPORTED`, see section 8.3) and logs an error that names the function and the argument. Functions that take no context have no log callback, and report failures through the result code only. Mistakes that cannot be detected cheaply, such as dangling handles or calls from the wrong thread, are undefined behaviour. Debug builds detect what they can.

### 6.7 State after failure

A function that fails leaves the objects it was called on unchanged, unless it documents otherwise. The library remains usable after any failure, including `VGL_RESULT_OUT_OF_MEMORY`.

---

## 7. Results and errors

### 7.1 Result codes

`vgl_result` is a 32-bit enumeration. Zero is success, positive values are non-error statuses, and negative values are errors:

```c
#define VGL_SUCCEEDED(result) ((result) >= 0)
#define VGL_FAILED(result) ((result) < 0)
```

| Value | Name | Meaning |
|---|---|---|
| 0 | `VGL_RESULT_SUCCESS` | The function succeeded. |
| 1 | `VGL_RESULT_INCOMPLETE` | The function succeeded, but an output array was too small to hold all elements (section 6.3). |
| 2 | `VGL_RESULT_NOT_READY` | The function succeeded, but the requested data is not loaded yet. The output holds the best available answer, as documented. |
| -1 | `VGL_RESULT_INVALID_ARGUMENT` | An argument is null, out of range or otherwise invalid. |
| -2 | `VGL_RESULT_OUT_OF_MEMORY` | An allocation failed, or a memory budget does not allow it. |
| -3 | `VGL_RESULT_NOT_SUPPORTED` | The request is valid, but this build, backend or device does not support it. |
| -4 | `VGL_RESULT_INVALID_OPERATION` | The function is not allowed in the current state of the object. |
| -5 | `VGL_RESULT_NOT_FOUND` | A requested item, file or tile does not exist. |
| -6 | `VGL_RESULT_IO_ERROR` | Reading data failed. |
| -7 | `VGL_RESULT_CORRUPT_DATA` | Data was read but failed validation or decoding. |
| -8 | `VGL_RESULT_VERSION_MISMATCH` | A pack, interface or structure version is not supported by this binary. |
| -9 | `VGL_RESULT_CANCELLED` | An asynchronous operation was cancelled (section 12). |
| -10 | `VGL_RESULT_BACKEND_ERROR` | A graphics API or engine call failed. |
| -11 | `VGL_RESULT_INTERNAL_ERROR` | A bug in the library. Please report it. |

New codes may be added in minor versions. Hosts must treat an unknown negative code as a generic failure and an unknown positive code as success. Each function documents the codes it can return.

### 7.2 Result names

```c
vgl_result vgl_result_name_get(vgl_result result, const char** out_name);
```

Writes the name of the code, such as `"VGL_RESULT_OUT_OF_MEMORY"`, as a static string that is valid for the lifetime of the process. For an unknown code it writes `"VGL_RESULT_UNKNOWN"` and returns `VGL_RESULT_NOT_FOUND`. It is free-threaded and needs no context.

### 7.3 Error details

The result code says what kind of failure occurred. The details go to the log callback (section 13): every failure of a function that has a context logs exactly one message at error level that names the function, says what was wrong, and says how to fix it. There is no "last error" state, which would be global or thread-local state (NFR-06).

### 7.4 No exceptions and no termination

No C++ exception crosses the API boundary (NFR-05), and the library never calls `abort`, `exit` or similar functions in response to host input, data or resource exhaustion. Internal invariant checks may stop the process in debug builds only.

---

## 8. Versioned structures

Structures passed across the API can grow in later versions without breaking hosts compiled against older headers (design document, section 13.3).

### 8.1 Layout and initialization

Every structure that may grow starts with a `uint32_t struct_size` field, which the caller sets to `sizeof` the structure. Every other field has a default that its zero value selects. The idiomatic initialization in C is therefore:

```c
vgl_context_create_info info = {.struct_size = sizeof(info)};
info.cpu_budget = 512u * 1024u * 1024u;
```

The designated initializer zeroes the other fields like `{sizeof(info)}` does, but compilers do not warn about it with `-Wmissing-field-initializers` (part of `-Wextra`).

Structures that are fixed by nature, such as `vgl_dvec3`, have no `struct_size` field and never change.

### 8.2 Layout rules

- Fields are only ever appended. Existing fields are never removed, reordered, renamed or changed in type. A deprecated field keeps its place and is ignored.
- Structures have no implicit padding, including at the end. Where alignment would insert padding, the header declares an explicit `uint32_t reserved` field that must be zero. Tests check the size of each public structure.
- In a structure that also holds 64-bit fields, each pointer is followed by `VGL_POINTER_PADDING(name)` from `vgl/base.h`, which adds a `uint32_t` on targets with 32-bit pointers and nothing elsewhere. Every pointer then takes 8 bytes, and the layout is the same on 32-bit and 64-bit targets.
- Arrays are passed as a pointer and a count: `const vgl_layer_info* layers; uint32_t layer_count;`.
- A versioned structure inside another one is referenced by pointer, not embedded by value, so that each can grow independently.

### 8.3 How the library reads input structures

- If `struct_size` is smaller than the size of the first published version of the structure, the call fails with `VGL_RESULT_INVALID_ARGUMENT`.
- If `struct_size` is smaller than the size the library knows, the host was compiled against an older header, and the missing fields take their defaults.
- If `struct_size` is larger than the size the library knows, the host was compiled against a newer header. If all the unknown trailing bytes are zero, the host uses none of the newer fields and the call proceeds. Otherwise it fails with `VGL_RESULT_NOT_SUPPORTED` and logs which structure and version are involved.

### 8.4 Output structures

Structures the library fills in, such as statistics, follow the same layout. The caller sets `struct_size`, and the library writes only the fields that fit within it.

---

## 9. Callbacks and extension interfaces

### 9.1 Single callbacks

A single callback is a function pointer type named `vgl_<name>_fn`. Its first parameter is always `void* user_data`. It is registered together with its user data, either as two fields of a structure or as two parameters of a function:

```c
typedef void (*vgl_log_fn)(void* user_data, vgl_log_level level, const char* message);
```

### 9.2 Extension interfaces

Extension points (design document, section 13.1) are tables of function pointers:

```c
#define VGL_PROVIDER_INTERFACE_VERSION 1

typedef struct vgl_provider_interface
{
    uint32_t version; /* VGL_PROVIDER_INTERFACE_VERSION */
    uint32_t reserved;
    void* user_data;
    vgl_result (*tile_read)(void* user_data, const vgl_tile_request* request);
    void (*destroy)(void* user_data); /* Optional. */
} vgl_provider_interface;
```

- The first field is `uint32_t version`, set to the `VGL_<NAME>_INTERFACE_VERSION` macro of the header the extension was compiled against. It takes the place of `struct_size`: the library knows the layout and semantics of every version it supports, and supports every version within its major version.
- New versions append entries. They may also change the documented semantics of existing entries, which the version number makes detectable.
- Optional entries may be null and are marked as such in the header. Required entries that are null cause registration to fail with `VGL_RESULT_INVALID_ARGUMENT`.
- The library copies the table when it is registered, so the host may keep it on the stack. The `user_data` pointer is retained.
- If the table has a `destroy` entry, the library calls it exactly once, when it no longer uses the extension.

The built-in backends, providers and decoders are implemented through the same interfaces.

### 9.3 Rules for callbacks

- A callback must not throw a C++ exception or `longjmp` out of the call.
- A callback must not call back into the library on the same context, except for functions documented as allowed from callbacks, such as `vgl_io_request_complete`.
- Each callback documents on which thread it may be called (section 11) and whether it may block.

---

## 10. Memory

### 10.1 Allocator

```c
#define VGL_ALLOCATOR_INTERFACE_VERSION 1

typedef struct vgl_allocator_interface
{
    uint32_t version;
    uint32_t reserved;
    void* user_data;
    void* (*allocate)(void* user_data, size_t size, size_t alignment);
    void (*deallocate)(void* user_data, void* memory, size_t size, size_t alignment);
} vgl_allocator_interface;
```

- `alignment` is a power of two. `allocate` returns null on failure, which the library reports as `VGL_RESULT_OUT_OF_MEMORY`.
- `deallocate` receives the same size and alignment that were passed to `allocate`. It is never called with a null pointer. (The entry is not named `free`, because the debug heap of the Microsoft C runtime defines `free` as a macro.)
- The allocator is supplied when the context is created. If it is not, the library uses the C runtime heap.
- Every allocation made on behalf of a context, and of the objects created from it, goes through that context's allocator (NFR-03). Functions that take no context do not allocate.
- Libraries that work on behalf of a context from outside the core, such as the backends, get the allocator with `vgl_context_allocator_get`: the host's, or one that uses the C heap. Libraries that take no context, such as the defaults module, take an allocator of their own.
- If the host runs library work on several threads (section 11), the allocator must be thread-safe.

### 10.2 Ownership

Memory allocated by the library is freed by the library, and memory allocated by the host is freed by the host. The host never calls `free` on a pointer it received from the library. Ownership is transferred only where a function says so explicitly (section 6.5).

### 10.3 Budgets

Memory budgets (NFR-04) are `uint64_t` byte counts. A zero budget selects the documented default.

---

## 11. Threading

The library never creates threads (NFR-02). Every function documents one of these thread-safety categories, using these exact terms:

| Category | Rule |
|---|---|
| **Free-threaded** | May be called from any thread, concurrently with any other call. |
| **Context-synchronized** | Calls on the same context, or on any object created from it, must not overlap. The host may make them from any thread, one at a time. Different contexts are independent. This is the default. |
| **Render thread** | Context-synchronized, and must also be called on the thread that owns the host's graphics device or context. Applies to backend functions and to GPU uploads. |

Callbacks document where they are invoked:

| Category | Rule |
|---|---|
| **Calling thread** | Invoked synchronously, inside the library function that triggers it. |
| **Task thread** | Invoked from work the host runs through the task callback, on any host thread, possibly concurrently with other task threads. |
| **Any thread** | May be invoked from either of the above. |

The log callback and the allocator are "any thread" and must be thread-safe if a task callback is installed.

---

## 12. Asynchronous operations

I/O and background work are asynchronous (design document, section 7.5). They follow one pattern:

1. The library starts an operation by calling a host callback, passing an opaque request handle (for example `vgl_io_request*`) and a description of the work.
2. The host completes the request exactly once, by calling the matching `complete` function (for example `vgl_io_request_complete`) with a result code and any output data. Completion functions are free-threaded, may be called from inside the starting callback, and only queue the result. Their effects become visible at the next `vgl_context_update`.
3. The library may cancel a request through an optional `cancel` callback. The host must still complete a cancelled request, either normally or with `VGL_RESULT_CANCELLED`.
4. A request handle is valid from the start callback until the host's completion call returns.

A context may be destroyed while requests are outstanding. It asks the host to cancel them through the `cancel` callback, if there is one, and the host still completes each of them, now or later; the library then discards the completion and frees the request. Neither side waits for the other, except that destroying a context waits for tasks that are running on host threads. The interfaces' `destroy` callbacks are called during context destruction, possibly before every request is completed, so a host that frees its I/O state there must first complete what it still holds. The details of each operation are specified with the foundation module (`vgl/foundation.h`).

---

## 13. Logging

```c
typedef void (*vgl_log_fn)(void* user_data, vgl_log_level level, const char* message);
```

- The log callback and the minimum level are set when the context is created. Without a callback, the library is silent: it never writes to standard output, standard error or a debugger itself.
- The default minimum level is `VGL_LOG_LEVEL_WARNING`.
- `message` is UTF-8 without a trailing newline, and is valid only during the call.
- Messages start with the name of the API function involved, say what happened, and say how to fix it where the fix is known. For example: `vgl_context_create: info->gpu_budget (1 MiB) is below the minimum of 16 MiB; raise it or set it to 0 for the default.`
- Libraries that work on behalf of a context, such as the backends, log through it with `vgl_context_log`, so that their messages reach the same callback, filtered by the same level, and follow the same rules.

---

## 14. Versioning and ABI

### 14.1 Version information

`vgl/base.h` defines `VGL_VERSION_MAJOR`, `VGL_VERSION_MINOR`, `VGL_VERSION_PATCH` and `VGL_VERSION_STRING` for the headers, and `vgl_version_get` reports the version of the binary. A host that needs a feature from a specific version checks the binary version at runtime.

### 14.2 Compatibility rules

From version 1.0, a host compiled against headers of version X.Y runs unchanged with a binary of version X.Z for any Z ≥ Y. Before 1.0, the minor version must match, and breaking changes are listed in the changelog (design document, section 13.3).

Within a major version, the following must not change:

- the name, parameters or return type of an exported function,
- the value or meaning of an enumeration value, flag bit or constant,
- the layout of a structure, except by appending fields (section 8),
- the layout of an interface version that has been published (section 9.2).

Exported functions are never removed within a major version.

### 14.3 Symbols and calling convention

- Exported functions are marked `VGL_API` and all start with `vgl_`. No other symbols are exported.
- When vgl is built as a shared library, consumers define `VGL_SHARED`. The CMake package and the pkg-config file do this automatically.
- Functions and callbacks use the platform's default C calling convention.

### 14.4 Deprecation

Deprecated functions, types and values are marked with `VGL_DEPRECATED("use <replacement>")`, which expands to the compiler's deprecation attribute, or to nothing where there is none. They remain available until the next major version.

---

## 15. Documentation comments

Every public declaration has a Doxygen comment. A function comment covers the following, in this order:

```c
/**
 * Queries the terrain height at a geodetic position.
 *
 * Uses the highest-resolution elevation currently loaded. Returns VGL_RESULT_NOT_READY if
 * finer data is available but not loaded yet; out_height then holds the best current value.
 *
 * @param context     The context to query.
 * @param latitude    Geodetic latitude in radians, in [-pi/2, pi/2].
 * @param longitude   Longitude in radians, in [-pi, pi].
 * @param out_height  Receives the ellipsoidal height in metres.
 *
 * @retval VGL_RESULT_SUCCESS           The height was written.
 * @retval VGL_RESULT_NOT_READY         A coarser height was written; finer data is loading.
 * @retval VGL_RESULT_INVALID_ARGUMENT  context or out_height is null, or an angle is out of range.
 *
 * @par Thread safety
 * Context-synchronized.
 */
```

The thread-safety category, every result code the function can return, and the lifetime of any returned pointer must be documented.

---

## 16. C++ wrapper

The wrapper is header-only, lives in namespace `vgl`, and calls only the C API, so it adds no ABI (design document, section 7.1).

- **Names** map mechanically from C. The prefix is dropped and the rest stays in snake case: `vgl_context` becomes `vgl::context`, `vgl_context_statistics_get(context, ...)` becomes `context.statistics_get(...)`, and `VGL_RESULT_OUT_OF_MEMORY` becomes `vgl::result::out_of_memory`.
- **Handles** become move-only RAII classes that destroy the object in their destructor. `get()` returns the C handle and `release()` gives up ownership.
- **Errors** are returned, never thrown, so the wrapper works with exceptions disabled. Functions that produce a value return `vgl::expected<T>`, which is `std::expected<T, vgl::result>` when the standard library provides it and a small equivalent otherwise. Other functions return `vgl::result`. A function whose C counterpart can fail only on arguments that the C++ signature rules out, such as null pointers, returns its value directly, for example `vgl::ellipsoid_wgs84_get()`. All of them are `[[nodiscard]]`.
- **Arrays** use `std::span` in place of pointer and count pairs. The two-call pattern of section 6.3 is wrapped in functions that return a container.
- **Strings** are passed as `const char*`, because the C API needs null-terminated strings.
- **Versioned structures** are the C structures themselves. `vgl::init<T>()` returns a zero-initialized `T` with `struct_size` set.
- **Extension interfaces** have abstract base classes with virtual functions. An adapter fills in the C table with `user_data` pointing at the C++ object, so extensions can be written as ordinary classes without RTTI.

---

## 17. Review checklist

For every new or changed public declaration:

- [ ] Names follow section 3.
- [ ] The function returns `vgl_result`, with parameters ordered object, inputs, outputs.
- [ ] Outputs use `out_` or `inout_`, and failure leaves them in the state section 6.2 requires.
- [ ] Only the types of section 4 are used: no `bool`, `long` or bit fields, and angles in radians.
- [ ] New structures that may grow start with `struct_size`, every zero value is a valid default, and there is no implicit padding.
- [ ] Enumerations have explicit values, a zero default where used in structures, and a `MAX_ENUM` value.
- [ ] Callbacks take `void* user_data` first and document their thread.
- [ ] The Doxygen comment documents the thread-safety category, every result code and pointer lifetimes.
- [ ] The change is ABI-compatible (section 14.2), or is listed under **Breaking** in the changelog.
- [ ] The C++ wrapper is updated, and the pure-C sample still reads naturally.

---

## Appendix A: Decisions and alternatives

| Decision | Alternative considered | Reason |
|---|---|---|
| `struct_size` as the first field of growable structures | A type tag and extension chain (`sType` and `pNext`, as in Vulkan) | A size field is simpler to use from C and sufficient for append-only growth. A chain can still be added later as an appended field. |
| Interfaces carry a version number instead of a size | `struct_size` on interfaces as well | Interfaces can change semantics as well as layout, and the design document asks for a version number per interface (section 13.1). |
| Destroy functions return `vgl_result` | Returning `void` | The design document requires a result code from every function. Destroy always succeeds, so callers may ignore it. |
| Negative error codes, positive statuses | All non-zero values as errors | Allows success with additional information (`INCOMPLETE`, `NOT_READY`) without a separate output. |
| Angles in radians | Degrees, which many users have at hand | One unit throughout avoids conversion bugs. Degree conversions are provided. |
| Column-major matrices with column vectors | Row-major | Matches GLM and OpenGL. The memory layout is also what row-major, row-vector Direct3D math produces. |
| `vgl_bool` as `uint32_t` | `bool` from `<stdbool.h>` | The size of `bool` is not guaranteed across compilers and languages, which matters for bindings. |
| Destroying a parent destroys its children | Failing, or leaking the children | Avoids leaks and dangling GPU resources on shutdown, while the warning still points out the host bug. |
| No last-error state | A thread-local last-error string | Would be global state (NFR-06). The log callback carries the details instead. |
