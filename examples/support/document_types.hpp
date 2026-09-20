#pragma once

#include <iostream>
#include <string>

#include <vng/content/document.hpp>

namespace example::document {

struct Pulse final {
    float frequency;
    float minimum;
};

struct ReactorSettings final {
    std::string id;
    std::string mesh;
    float emission;
    Pulse pulse;
};

struct DoorSettings final {
    std::string id;
    vng::Vec3 travel;
    float opening_time;
};

// Place this optional overload alongside the application type. get<Pulse>()
// discovers it through ADL; the content library never needs to know about Pulse.
inline vng::content::Result<Pulse> decode(
    vng::content::NodeView node, vng::content::Type<Pulse>)
{
    return node.read([](vng::content::Reader& r) {
        const auto frequency = r.get<float>("frequency");
        if (frequency <= 0.0f) {
            r.child("frequency").fail("Pulse frequency must be positive");
        }
        return Pulse{
            .frequency = frequency,
            .minimum = r.get_or<float>("minimum", 0.0f),
        };
    });
}

inline void print_diagnostic(
    const vng::content::Diagnostic& diagnostic, std::ostream& out = std::cerr)
{
    out << (diagnostic.path.empty() ? "<document>" : diagnostic.path.string());
    if (diagnostic.location) {
        out << ':' << diagnostic.location->line << ':' << diagnostic.location->column;
    }
    out << ": ";
    if (!diagnostic.property_path.empty()) {
        out << diagnostic.property_path << ": ";
    }
    out << diagnostic.message << '\n';
}

inline int fail(const vng::content::Diagnostic& diagnostic)
{
    print_diagnostic(diagnostic);
    return 1;
}

} // namespace example::document
