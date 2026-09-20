#pragma once

#include "../file_mesh_types.hpp"
#include "mesh_draw.hpp"
#include <vng/content/vmesh_schema.hpp>
#include <vng/render/opengl_program_runtime.hpp>
#include <vng/shader/shader.hpp>

namespace editor_example::mesh_shading {
using namespace vng;
using file_mesh_example::Position;
using file_mesh_example::Color;
using file_mesh_example::Vertex;
using file_mesh_example::SurfaceColor;
struct Normal : gfx::Semantic<Vec3> {};
struct Emission : gfx::Semantic<f32> {};
struct WorldPosition : gfx::Semantic<Vec3> {};
struct WorldNormal : gfx::Semantic<Vec3> {};
// Keep editable positions/colors in their original compact stream. A pointer
// drag need not upload unchanged shading attributes, even for large ships.
using Surface = gfx::Record<Normal, Emission>;
using Mesh = gfx::Mesh<Vertex, Surface>;
using Program = render::TypedOpenGLProgramRuntime<Mat4, Lighting>;
enum class LightingStyle { standard, illustrated };
inline LightingStyle lighting_style(const content::vmesh::Document& document) {
    const auto found=document.metadata.find("render/lighting");
    return found!=document.metadata.end() && found->second=="illustrated"
        ? LightingStyle::illustrated : LightingStyle::standard;
}

inline dsl::Float3 safe_normalize(dsl::Float3 value) {
    return value / dsl::sqrt(dsl::max(dsl::dot(value, value), 1.0e-12F));
}
// Inverse transpose for the editor's positive TRS transforms. Dividing each
// model column by its squared length removes nonuniform scale correctly.
inline dsl::Float3 transform_normal(dsl::Float4x4 model,dsl::Float3 normal) {
    const auto x=(model*Vec4{1,0,0,0}).xyz(), y=(model*Vec4{0,1,0,0}).xyz(), z=(model*Vec4{0,0,1,0}).xyz();
    return x*(normal.x()/dsl::max(dsl::dot(x,x),1.0e-12F)) +
           y*(normal.y()/dsl::max(dsl::dot(y,y),1.0e-12F)) +
           z*(normal.z()/dsl::max(dsl::dot(z,z),1.0e-12F));
}

template<class Stage>
inline auto shade(Stage& s, dsl::Expr<Lighting> lights, LightingStyle style=LightingStyle::standard) {
    const auto world = s.input(WorldPosition{});
    const auto source_normal = s.input(WorldNormal{});
    const auto normal = safe_normalize(source_normal);
    const auto eye = safe_normalize(lights.get(Eye{}) - world);
    const auto source = lights.get(Light{});
    const auto light = safe_normalize(source.xyz() - world * source.w());
    const auto diffuse = dsl::max(dsl::dot(normal, light), 0.0F);
    const auto fill = dsl::max(dsl::dot(normal,
        s.constant(Vec3{.62F, .66F, .42F})), 0.0F);
    const auto half_vector = safe_normalize(light + eye);
    const auto specular = dsl::max(dsl::dot(normal, half_vector), 0.0F);
    const auto specular2 = specular * specular;
    const auto specular4 = specular2 * specular2;
    const auto specular8 = specular4 * specular4;
    const auto specular16 = specular8 * specular8;
    const auto specular32 = specular16 * specular16;
    const auto color = s.input(Color{}).xyz();
    const auto sunlight = s.constant(Vec3{2.8F, 1.95F, 1.2F});
    auto lit = color * (s.constant(Vec3{.09F, .13F, .21F})
        + sunlight * diffuse + s.constant(Vec3{.07F, .12F, .23F}) * fill)
        + sunlight * (specular32 * .22F);
    if(style==LightingStyle::illustrated) {
        // This is a host-time code-emission choice, not a per-fragment branch.
        // Broad lighting bands, cool shadows, restrained gloss and a thin rim.
        const auto facing=dsl::dot(normal,light);
        const auto ease=[](dsl::Float x) {
            const auto t=dsl::clamp(x,0.F,1.F);return t*t*(3.F-2.F*t);
        };
        const auto day=ease((facing+.12F)/.34F);
        const auto high=ease((facing-.55F)/.15F);
        const auto rim=1.F-dsl::max(dsl::dot(normal,eye),0.F);
        const auto rim2=rim*rim, rim4=rim2*rim2;
        lit=color*(s.constant(Vec3{.07F,.16F,.32F})+s.constant(Vec3{.85F,1.03F,1.04F})*day+
            s.constant(Vec3{.22F,.24F,.23F})*high)+s.constant(Vec3{.015F,.22F,.5F})*rim4*(.25F+.75F*day);
    }
    // Zero normals opt out of lighting. Old colored meshes retain exactly
    // their former unlit output rather than acquiring arbitrary normals.
    const auto surface = dsl::select(dsl::dot(source_normal, source_normal) > 1.0e-12F,
        lit, color);
    const auto radiance = dsl::vec4((surface + color * dsl::max(s.input(Emission{}), 0.0F))
        * lights.get(Brightness{}), 1.0F);
    s.observe(SurfaceColor{}, radiance);
    s.observe(WorldPosition{}, world);
    s.observe(WorldNormal{}, normal);
    return s.output(dsl::field<shader::Color<0>>(radiance));
}

inline auto shader_program(LightingStyle style=LightingStyle::standard) {
    using VI = shader::VertexInputs<Position, Color, Normal, Emission>;
    using VO = shader::VertexOutputs<shader::ClipPosition, shader::smooth<Color>,
        shader::smooth<WorldPosition>, shader::smooth<WorldNormal>, shader::smooth<Emission>>;
    using FI = shader::FragmentInputs<shader::smooth<Color>, shader::smooth<WorldPosition>,
        shader::smooth<WorldNormal>, shader::smooth<Emission>>;
    using FO = shader::FragmentOutputs<shader::Color<0>>;
    auto vertex = shader::vertex<VI, VO>("editor_mesh", [](auto& s, dsl::Float4x4 model) {
        const auto world = model * dsl::vec4(s.input(Position{}), 1.0F);
        const auto normal = transform_normal(model,s.input(Normal{}));
        return s.output(dsl::field<shader::ClipPosition>(s.camera().project(world.xyz())),
            dsl::field<Color>(s.input(Color{})), dsl::field<WorldPosition>(world.xyz()),
            dsl::field<WorldNormal>(normal), dsl::field<Emission>(s.input(Emission{})));
    });
    auto fragment = shader::fragment<FI, FO>("editor_mesh", [style](auto& s, dsl::Expr<Lighting> lights) {
        return shade(s, lights, style);
    });
    using Linked = shader::Result<shader::TypedGraphicsProgram<Mat4, Lighting>>;
    if (!vertex) return Linked{std::unexpected(vertex.error())};
    if (!fragment) return Linked{std::unexpected(fragment.error())};
    return shader::link(std::move(*vertex), std::move(*fragment));
}

inline bool ignored_color(const content::vmesh::Document& source) {
    const auto color = std::ranges::find(source.vertex_fields, "color/0", &content::vmesh::VertexField::name);
    return color != source.vertex_fields.end() &&
        color->type != content::vmesh::FieldType{content::vmesh::ScalarType::Float32, 3} &&
        color->type != content::vmesh::FieldType{content::vmesh::ScalarType::Float32, 4};
}

inline content::Result<Mesh> cpu_mesh(const content::vmesh::Document& source) {
    auto schema = content::vmesh::schema<Vertex>();
    schema.map("position", Position{});
    schema.map("color/0", Color{});
    auto surface = content::vmesh::schema<Surface>();
    surface.map("normal", Normal{});
    surface.map("emission", Emission{});
    auto document = source;
    const auto color = std::ranges::find(document.vertex_fields, "color/0", &content::vmesh::VertexField::name);
    if (color == document.vertex_fields.end()) {
        document.vertex_fields.push_back({"color/0", {content::vmesh::ScalarType::Float32, 4},
            std::vector<f32>(document.vertex_count * 4, 1)});
    } else if (color->type == content::vmesh::FieldType{content::vmesh::ScalarType::Float32, 3}) {
        const auto& rgb = std::get<std::vector<f32>>(color->values);
        std::vector<f32> rgba;
        rgba.reserve(document.vertex_count * 4);
        for (std::size_t vertex = 0; vertex < document.vertex_count; ++vertex)
            rgba.insert(rgba.end(), {rgb[vertex * 3], rgb[vertex * 3 + 1], rgb[vertex * 3 + 2], 1.F});
        color->type.components = 4;
        color->values = std::move(rgba);
    } else if (ignored_color(source)) {
        color->type = {content::vmesh::ScalarType::Float32, 4};
        color->values = std::vector<f32>(document.vertex_count * 4, 1);
    }
    // Optional preview conventions never rewrite the authored document. A
    // missing/unsupported normal keeps a mesh unlit; emission defaults to zero.
    for (const auto& [name, count] : {std::pair{"normal", 3U}, std::pair{"emission", 1U}}) {
        const content::vmesh::FieldType type{content::vmesh::ScalarType::Float32, static_cast<u8>(count)};
        const auto field = std::ranges::find(document.vertex_fields, name, &content::vmesh::VertexField::name);
        if (field == document.vertex_fields.end())
            document.vertex_fields.push_back({name, type, std::vector<f32>(document.vertex_count * count, 0)});
        else if (field->type != type) {
            field->type = type;
            field->values = std::vector<f32>(document.vertex_count * count, 0);
        }
    }
    return content::vmesh::decode(document, schema, surface);
}
} // namespace editor_example::mesh_shading
