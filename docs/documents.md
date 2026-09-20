# Structured documents

`vng::content` provides a backend-independent document format and typed access
to arbitrary properties. It does not define a scene, object registry, material
system, renderer, or resource loader. Names such as `objects`, `kind`, `mesh`,
and `emission` have meaning only in the application consuming them.

Include `<vng/content/document.hpp>` and link `vng_content` (`vng::content`).
No window, graphics context, or GPU resource is needed.

## Read a property

```cpp
namespace content = vng::content;

auto document = content::read_document("room.vscene");
if (!document) return std::unexpected(document.error());

auto emission = document->root().get<float>("emission");
if (!emission) return std::unexpected(emission.error());
```

`content::Result<T>` is exactly `std::expected<T, content::Diagnostic>`, not
another wrapper. The low-level getters return errors directly:

| Operation | Return type | Meaning |
| --- | --- | --- |
| `node.as<T>()` | `Result<T>` | Decode this value. |
| `node.get<T>("name")` | `Result<T>` | Require and decode a named property. |
| `node.get_or<T>("name", fallback)` | `Result<T>` | Use the fallback only if absent. |
| `node.optional<T>("name")` | `Result<std::optional<T>>` | Return an empty optional only if absent. |
| `node.child("name")` | `Result<NodeView>` | Require a child, without decoding it. |
| `node.elements()` | `Result<ArrayView>` | Iterate array values. |
| `node.members()` | `Result<ObjectView>` | Iterate `{name, value}` members. |

A present `null` is not absence. A present malformed value never silently
becomes a fallback. Neither reading nor looking up a missing key inserts it.
`child`, `get`, `get_or`, and `optional` require their receiver to be an object.

Keys are literal strings. `get<float>("pulse.frequency")` reads a property
whose entire name is `pulse.frequency`; it is not a path expression. To
traverse, use `child("pulse")`, then `get<float>("frequency")`.

Built-in decoding supports booleans; signed and unsigned integral types;
floating-point types; owning `std::string`; `std::nullptr_t`; engine vectors
such as `Vec2`, `Vec3`, and `Vec4`; and `std::array<T, N>` / `std::vector<T>`
whose elements can themselves be decoded. Fixed-size sequences require the
exact element count. Heterogeneous arrays are allowed in documents but need
individual node access rather than a homogeneous vector conversion.

Numbers are range-checked. `6` can be read as `float`; a fractional number
cannot silently become an integer; negative values cannot become unsigned.
Floating conversions can round, but overflow and nonzero-to-zero underflow
are errors. Strings and booleans are not silently converted to numbers.
Numbers in files are stored as signed/unsigned 64-bit integers or finite
64-bit floating-point values, not arbitrary-precision numbers.

## Decode a description with less error-handling boilerplate

```cpp
struct ReactorSettings {
    std::string mesh;
    float emission;
    float frequency;
};

auto settings = object.read([](content::Reader& r) {
    return ReactorSettings{
        .mesh = r.get<std::string>("mesh"),
        .emission = r.get_or<float>("emission", 0.0f),
        .frequency = r.child("pulse").get<float>("frequency"),
    };
});
// settings: std::expected<ReactorSettings, content::Diagnostic>
```

Inside `read()`, `Reader` methods return plain values. The first invalid read
throws an internal diagnostic exception, caught by the surrounding `read()`;
decoding stops immediately instead of continuing with fabricated values. A
callback may return a plain value or `void`, but not a reference or `expected`.
`Document::read()` is shorthand for reading its root.

This is an optional convenience layer: low-level `NodeView` accessors do not
throw that control-flow exception. `read()` catches only its own diagnostic
failure, not unrelated exceptions thrown by application code or allocation.
It does not undo callback side effects. Decode a CPU description first, check
the result, then create resources or update live application state.

Use an explicitly typed `content::Reader&` parameter as above. A generic
`auto&` parameter makes member templates dependent in C++, requiring syntax
such as `r.template get<float>("emission")`.

The scoped reader also supports iteration and application validation:

```cpp
auto names = document->read([](content::Reader& r) {
    std::vector<std::string> result;
    for (auto object : r.child("objects").elements()) {
        result.push_back(object.get<std::string>("id"));
    }
    return result;
});
```

`r.fail("message")` reports a validation failure at that node. Use
`r.child("frequency").fail(...)` to associate it with a particular property.
`r.view()` exposes the corresponding low-level `NodeView` when needed.

## Extend decoding next to your type

```cpp
namespace game {

struct Pulse {
    float frequency;
    float minimum;
};

content::Result<Pulse> decode(content::NodeView node, content::Type<Pulse>)
{
    return node.read([](content::Reader& r) {
        return Pulse{
            .frequency = r.get<float>("frequency"),
            .minimum = r.get_or<float>("minimum", 0.0f),
        };
    });
}

} // namespace game

auto pulse = object.get<game::Pulse>("pulse");
```

Argument-dependent lookup discovers this free function in the custom type's
namespace. No registry, base class, central traits specialization, or field
macros are needed. It can also be implemented entirely with low-level
`expected` accessors. Return a diagnostic from `node.error(code, message)`
to preserve source context. A returned custom error missing source information
is supplemented from the decoded node; more precise child context is retained.

## Format 1.0

```text
vscene 1.0

# Application-defined data; the parser assigns these names no special meaning.
objects = [
    {
        id = "reactor";
        kind = "pulsing_mesh";
        emission = 6;
        pulse = { frequency = 1.5; minimum = 0.2; };
    },
    {
        id = "door";
        kind = "sliding_door";
        travel = [0, 2.5, 0];
        opening_time = 0.8;
    },
];
```

The header contains a bare document-kind identifier and version `1.0`.
`vscene` is only one possible kind; a consumer can check `document.kind()`.
Unknown versions are rejected. The root is an implicit object, with no outer
braces. Object members use `key = value;`; nested objects use braces, and
arrays use commas with an optional trailing comma.

Values are objects, arrays, quoted UTF-8 strings, decimal numbers, `true`,
`false`, or `null`. Strings support JSON-style escapes, including Unicode
escapes and valid surrogate pairs. Bare keys begin with an ASCII letter or
underscore; subsequent characters may also include digits, `.`, `/`, and `-`.
Other keys must be quoted. Duplicate properties in the same object are errors.
`#` introduces a line comment outside strings. There are no includes, macros,
expressions, environment substitutions, asset references, or reserved scene
property names. A path written in a property remains a string until the
application interprets it.

`Document` owns an immutable snapshot and is cheaply copyable through shared
ownership. `NodeView`, array/object ranges, member names, and strings obtained
with `borrow_string()` borrow that snapshot. They remain valid while any
owning copy of that document lives; moving the document does not invalidate
them. Do not retain a view after destroying the final owner.
`get<std::string>()` returns an owning string. A provider can retain a
`Document` snapshot or a file path to reload, rather than a dangling view.

## Errors, limits, and writing

Parsing and typed decoding preserve file path, one-based line/column, byte
offset, and a property path when available. For example, an invalid pulse
frequency is reported at `objects[0].pulse.frequency`. Missing-property
errors identify the requested path and its containing object's location.
`NodeView::kind()`, `location()`, and `property_path()` support inspection.

```cpp
auto document = content::parse_document(source, {
    .limits = {.max_source_bytes = 1024 * 1024, .max_depth = 32},
    .source_path = "memory-preview.vscene",
});
```

`DocumentLimits` bounds source bytes, decoded storage bytes, node count,
nesting depth, decoded string bytes, and number-token bytes. Defaults are
64 MiB source, 128 MiB decoded storage, 1,048,576 nodes, depth 128, 8 MiB per
string, and 256 bytes per number token. Depth is additionally capped at 256
even if a caller requests a higher limit. The writer accepts its own limits.
The decoded-byte budget accounts for stored nodes, child references, keys,
and string contents; it is not an exact process-heap ceiling (allocator
capacity, temporary working data, and diagnostics can take additional memory).
The number-token limit also applies to the required `1.0` version token, so
values below three bytes reject every document in both reading and writing.

```cpp
auto text = content::write_document(*document); // Result<std::string>
auto saved = content::write_document("copy.vscene", *document); // Result<void>
```

The writer serializes the complete snapshot, including properties no typed
loader understood, in deterministic canonical syntax. It preserves property
order and values, not original whitespace, comments, escapes, or number
spellings. This is a read-only document API with canonical round-trip writing,
not an editor mutation API or a typed-object serializer. File writing replaces
the destination; it is not an atomic-save transaction.

## CPU-only example

```sh
cmake --build build --target vng_document_demo
./build/vng_document_demo
./build/vng_document_demo examples/assets/custom_scene.vscene
```

[The example](../examples/document.cpp) shows explicit getters, scoped
decoding, a custom `Pulse` decoder, different object-specific settings,
property iteration, canonical round-trip preservation of unknown extensions,
and an intentionally generated source-aware type error. It does not create a
scene or resolve the sample mesh path. Actual scene conventions and rendering
can be built independently above this layer.
