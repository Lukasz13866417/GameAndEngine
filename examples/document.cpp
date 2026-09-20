#include <filesystem>
#include <iostream>
#include <string>

#include <vng/content/document.hpp>

#include "support/document_types.hpp"

int main(int argc, char** argv)
{
    namespace content = vng::content;
    using namespace example::document;

    if (argc > 2) {
        std::cerr << "Usage: vng_document_demo [document.vscene]\n";
        return 1;
    }
    const auto path = argc == 2 ? std::filesystem::path{argv[1]}
                               : std::filesystem::path{VNG_EXAMPLE_DOCUMENT_PATH};
    auto document = content::read_document(path);
    if (!document) return fail(document.error());

    // This is an application policy. The generic parser accepts other kinds.
    if (document->kind() != "vscene") {
        std::cerr << "This example expects a vscene document\n";
        return 1;
    }
    const auto root = document->root();

    // The low-level API returns expected: no exceptions for failed decoding.
    auto exposure = root.get<float>("exposure");
    if (!exposure) return fail(exposure.error());
    std::cout << "Document: " << path << "\nExposure: " << *exposure << '\n';

    auto objects_node = root.child("objects");
    if (!objects_node) return fail(objects_node.error());
    auto objects = objects_node->elements();
    if (!objects) return fail(objects.error());

    for (const auto object : *objects) {
        auto kind = object.get<std::string>("kind");
        if (!kind) return fail(kind.error());

        // Checked decoding removes repetitive error propagation. The callback
        // returns a plain CPU value; read() returns expected<Value, Diagnostic>.
        if (*kind == "pulsing_mesh") {
            auto settings = object.read([](content::Reader& r) {
                return ReactorSettings{
                    .id = r.get<std::string>("id"),
                    .mesh = r.get<std::string>("mesh"),
                    .emission = r.get_or<float>("emission", 0.0f),
                    .pulse = r.get<Pulse>("pulse"),
                };
            });
            if (!settings) return fail(settings.error());
            std::cout << "Reactor " << settings->id << ": " << settings->mesh
                      << ", emission " << settings->emission
                      << ", pulse " << settings->pulse.frequency << " Hz\n";
        } else if (*kind == "sliding_door") {
            auto settings = object.read([](content::Reader& r) {
                return DoorSettings{
                    .id = r.get<std::string>("id"),
                    .travel = r.get<vng::Vec3>("travel"),
                    .opening_time = r.get<float>("opening_time"),
                };
            });
            if (!settings) return fail(settings.error());
            const auto travel = settings->travel;
            std::cout << "Door " << settings->id << ": travel ["
                      << travel.x << ", " << travel.y << ", " << travel.z
                      << "], opens in " << settings->opening_time << " s\n";
        } else {
            std::cout << "Unrecognized object kind preserved: " << *kind << '\n';
        }

        auto members = object.members();
        if (!members) return fail(members.error());
        std::cout << "  Available properties:";
        for (const auto [name, value] : *members) {
            (void)value;
            std::cout << ' ' << name;
        }
        std::cout << '\n';
    }

    // Round-trip the whole document, not just the typed settings we understood.
    // This does not overwrite the input file. Comments/formatting are canonicalized.
    auto canonical = content::write_document(*document);
    if (!canonical) return fail(canonical.error());
    auto round_trip = content::parse_document(*canonical);
    if (!round_trip) return fail(round_trip.error());
    auto written_again = content::write_document(*round_trip);
    if (!written_again) return fail(written_again.error());
    if (*canonical != *written_again) {
        std::cerr << "Canonical document round trip was not stable\n";
        return 1;
    }
    std::cout << "\nCanonical round trip (including undecoded extensions):\n"
              << *canonical;

    // Deliberately request the wrong type to demonstrate source-aware errors.
    auto wrong_type = root.get<float>("objects");
    if (!wrong_type) {
        std::cout << "\nExample diagnostic (intentional):\n";
        print_diagnostic(wrong_type.error(), std::cout);
    }
}
