#include "mesh_colours.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace editor_example {
namespace {
namespace vm = vng::content::vmesh;
const vm::VertexField* colour_field(const vm::Document& document) {
    const auto found = std::ranges::find(document.vertex_fields, "color/0", &vm::VertexField::name);
    if (found == document.vertex_fields.end() || found->type.scalar != vm::ScalarType::Float32) return nullptr;
    if (found->type.components != 3 && found->type.components != 4) return nullptr;
    return &*found;
}
std::array<vng::f32, 4> colour_at(const std::vector<vng::f32>& values, std::size_t vertex, std::size_t components) {
    std::array<vng::f32, 4> colour{0, 0, 0, 1};
    for (std::size_t c = 0; c < components; ++c) colour[c] = values[vertex * components + c];
    return colour;
}
} // namespace

std::vector<MeshColour> mesh_palette(const vm::Document& document, std::size_t most) {
    const auto* field = colour_field(document);
    if (!field) return {};
    const auto& values = std::get<std::vector<vng::f32>>(field->values);
    // Colour -> palette index; equal counts keep first-appearance order, so
    // an entry stays in its place when it is recoloured.
    std::map<std::array<vng::f32, 4>, std::size_t> index;
    std::vector<MeshColour> palette;
    for (std::size_t v = 0; v < document.vertex_count; ++v) {
        const auto [entry, added] = index.try_emplace(colour_at(values, v, field->type.components), palette.size());
        if (added) {
            if (palette.size() == most) return {};
            palette.push_back({entry->first, 0});
        }
        ++palette[entry->second].vertices;
    }
    std::ranges::stable_sort(palette, std::greater{}, &MeshColour::vertices);
    return palette;
}

vng::content::Result<vm::Document> recolour(vm::Document document, const std::array<vng::f32, 4>& from, vng::Vec3 to) {
    const auto* found = colour_field(document);
    if (!found) {
        vng::content::Diagnostic error;
        error.code = vng::content::ErrorCode::invalid_document;
        error.message = "The mesh has no float RGB or RGBA color/0 field to recolour";
        return std::unexpected(std::move(error));
    }
    auto& field = document.vertex_fields[static_cast<std::size_t>(found - document.vertex_fields.data())];
    auto& values = std::get<std::vector<vng::f32>>(field.values);
    const std::size_t components = field.type.components;
    for (std::size_t v = 0; v < document.vertex_count; ++v)
        if (colour_at(values, v, components) == from) {
            values[v * components] = to.x;
            values[v * components + 1] = to.y;
            values[v * components + 2] = to.z;
        }
    return document;
}

vng::u32 to_srgb8(vng::f32 linear) {
    const auto c = std::clamp(linear, 0.F, 1.F);
    const auto srgb = c <= .0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.F / 2.4F) - .055F;
    return static_cast<vng::u32>(std::lround(srgb * 255.F));
}

vng::f32 from_srgb8(vng::u32 srgb) {
    const auto c = static_cast<vng::f32>(std::min(srgb, 255U)) / 255.F;
    return c <= .04045F ? c / 12.92F : std::pow((c + .055F) / 1.055F, 2.4F);
}
} // namespace editor_example
