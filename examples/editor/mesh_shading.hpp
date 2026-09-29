#pragma once

#include "../file_mesh_types.hpp"
#include "mesh_draw.hpp"
#include "tunnel_haze.hpp"
#include "../support/earth_tunnel_sizes.hpp"
#include <vng/content/vmesh_schema.hpp>
#include <vng/render_opengl/program_runtime.hpp>
#include <vng/shader/shader.hpp>

namespace editor_example::mesh_shading {
using file_mesh_example::Position;
using file_mesh_example::Color;
using file_mesh_example::Vertex;
using file_mesh_example::SurfaceColor;
struct Normal : vng::gfx::Semantic<vng::Vec3> {};
struct Emission : vng::gfx::Semantic<vng::f32> {};
struct WorldPosition : vng::gfx::Semantic<vng::Vec3> {};
struct WorldNormal : vng::gfx::Semantic<vng::Vec3> {};
// Keep editable positions/colors in their original compact stream. A pointer
// drag need not upload unchanged shading attributes, even for large ships.
using Surface = vng::gfx::Record<Normal, Emission>;
using Mesh = vng::gfx::Mesh<Vertex, Surface>;
using Program = vng::render::TypedOpenGLProgramRuntime<vng::Mat4, Lighting>;
// Matte: rough surfaces such as bare rock, diffuse without a highlight.
// Regolith: illustrated night lighting for airless ground, whose faint rim
// does not glow like an atmosphere at grazing angles.
enum class LightingStyle { standard, illustrated, illustrated_night, tunnel, tunnel_departure, tunnel_departure_night, matte,
                           regolith };
inline LightingStyle lighting_style(const vng::content::vmesh::Document& document) {
    const auto found=document.metadata.find("render/lighting");
    const auto emission=document.metadata.find("render/emission");
    if(found!=document.metadata.end() && found->second=="tunnel")return LightingStyle::tunnel;
    if(found!=document.metadata.end() && found->second=="matte")return LightingStyle::matte;
    if(found!=document.metadata.end() && found->second=="regolith")return LightingStyle::regolith;
    if(found!=document.metadata.end() && found->second=="tunnel_departure_night")return LightingStyle::tunnel_departure_night;
    if(found!=document.metadata.end() && found->second=="tunnel_departure")return LightingStyle::tunnel_departure;
    if(found!=document.metadata.end()&&found->second=="illustrated"&&
       emission!=document.metadata.end()&&emission->second=="night")return LightingStyle::illustrated_night;
    return found!=document.metadata.end() && found->second=="illustrated"
        ? LightingStyle::illustrated : LightingStyle::standard;
}

inline vng::dsl::Float3 safe_normalize(vng::dsl::Float3 value) {
    return value / vng::dsl::sqrt(vng::dsl::max(vng::dsl::dot(value, value), 1.0e-12F));
}
// Inverse transpose for the editor's positive TRS transforms. Dividing each
// model column by its squared length removes nonuniform scale correctly.
inline vng::dsl::Float3 transform_normal(vng::dsl::Float4x4 model,vng::dsl::Float3 normal) {
    const auto x=(model*vng::Vec4{1,0,0,0}).xyz(), y=(model*vng::Vec4{0,1,0,0}).xyz(), z=(model*vng::Vec4{0,0,1,0}).xyz();
    return x*(normal.x()/vng::dsl::max(vng::dsl::dot(x,x),1.0e-12F)) +
           y*(normal.y()/vng::dsl::max(vng::dsl::dot(y,y),1.0e-12F)) +
           z*(normal.z()/vng::dsl::max(vng::dsl::dot(z,z),1.0e-12F));
}

template<class Stage>
inline auto shade(Stage& s, vng::dsl::Expr<Lighting> lights, LightingStyle style=LightingStyle::standard) {
    const auto world = s.input(WorldPosition{});
    const auto source_normal = s.input(WorldNormal{});
    const auto normal = safe_normalize(source_normal);
    const auto eye = safe_normalize(lights.get(Eye{}) - world);
    const auto source = lights.get(Light{});
    const auto light = safe_normalize(source.xyz() - world * source.w());
    const auto diffuse = vng::dsl::max(vng::dsl::dot(normal, light), 0.0F);
    const auto fill = vng::dsl::max(vng::dsl::dot(normal,
        s.constant(vng::Vec3{.62F, .66F, .42F})), 0.0F);
    const auto half_vector = safe_normalize(light + eye);
    const auto specular = vng::dsl::max(vng::dsl::dot(normal, half_vector), 0.0F);
    const auto specular2 = specular * specular;
    const auto specular4 = specular2 * specular2;
    const auto specular8 = specular4 * specular4;
    const auto specular16 = specular8 * specular8;
    const auto specular32 = specular16 * specular16;
    const auto color = s.input(Color{}).xyz();
    const auto sunlight = s.constant(vng::Vec3{2.8F, 1.95F, 1.2F});
    auto lit = color * (s.constant(vng::Vec3{.09F, .13F, .21F})
        + sunlight * diffuse + s.constant(vng::Vec3{.07F, .12F, .23F}) * fill);
    // A white highlight independent of albedo reads as a wet glint on rock.
    if (style != LightingStyle::matte) lit = lit + sunlight * (specular32 * .22F);
    const bool night_side=style==LightingStyle::illustrated_night || style==LightingStyle::tunnel_departure_night ||
        style==LightingStyle::regolith;
    if(style==LightingStyle::illustrated || night_side) {
        // This is a host-time code-emission choice, not a per-fragment branch.
        // Broad lighting bands, cool shadows, restrained gloss and a thin rim.
        const auto facing=vng::dsl::dot(normal,light);
        const auto ease=[](vng::dsl::Float x) {
            const auto t=vng::dsl::clamp(x,0.F,1.F);return t*t*(3.F-2.F*t);
        };
        const auto day=ease((facing+.12F)/.34F);
        const auto high=ease((facing-.55F)/.15F);
        const auto rim=1.F-vng::dsl::max(vng::dsl::dot(normal,eye),0.F);
        const auto rim2=rim*rim, rim4=rim2*rim2;
        const auto shadow=night_side?vng::Vec3{.018F,.045F,.10F}:vng::Vec3{.07F,.16F,.32F};
        const auto rim_strength=style==LightingStyle::regolith?.25F:1.F;
        lit=color*(s.constant(shadow)+s.constant(vng::Vec3{.85F,1.03F,1.04F})*day+
            s.constant(vng::Vec3{.22F,.24F,.23F})*high)+s.constant(vng::Vec3{.015F*rim_strength,.22F*rim_strength,.5F*rim_strength})*rim4*(.25F+.75F*day);
    }
    // Zero normals opt out of lighting. Old colored meshes retain exactly
    // their former unlit output rather than acquiring arbitrary normals.
    const auto surface = vng::dsl::select(vng::dsl::dot(source_normal, source_normal) > 1.0e-12F,
        lit, color);
    auto emission=vng::dsl::max(s.input(Emission{}),0.F);
    if(night_side) {
        // Smooth twilight transition, evaluated against the scene light. The
        // same neutral DSL is used by instanced and diagnostic render paths.
        const auto night=vng::dsl::clamp((.12F-vng::dsl::dot(normal,light))/.30F,0.F,1.F);
        emission=emission*(.025F+.975F*night*night*(3.F-2.F*night));
    }
    auto hdr = (surface + color * emission) * lights.get(Brightness{});
    if(style==LightingStyle::tunnel || (style==LightingStyle::tunnel_departure || style==LightingStyle::tunnel_departure_night)) {
        // Kilometre-scale aerial perspective. Distance is evaluated per pixel,
        // not baked into the mesh: moving the editor camera remains correct.
        // The near-white distant veil suggests light scattered down the bore, not
        // a visible far portal. This is an art-directed single-scatter proxy,
        // not a volumetric light/shadow simulation.
        const auto delta=lights.get(Eye{})-world;
        // Leave nearby panel seams / plane changes readable. Deeper in the
        // bore the white scattered light fills the whole view.
        auto optical_depth=tunnel_haze::depth(vng::dsl::dot(delta,delta));
        if(style==LightingStyle::tunnel) {
            // The slow study bends in X/Z. Its interior haze must not turn
            // the same shell white when viewed from an exterior editor camera.
            const auto eye=lights.get(Eye{});
            const auto z=eye.z();
            const auto center_x=z*z/(6371.F+vng::dsl::sqrt(vng::dsl::max(6371.F*6371.F-z*z,.001F)));
            const auto radial2=(eye.x()-center_x)*(eye.x()-center_x)+eye.y()*eye.y();
            optical_depth=optical_depth*vng::dsl::select(radial2<s.constant(example::earth::cinematic_tunnel_radius_km*example::earth::cinematic_tunnel_radius_km),1.F,0.F)
                *vng::dsl::select(z<12.F,1.F,0.F)*vng::dsl::select(z>-1000.F,1.F,0.F);
        }
        if((style==LightingStyle::tunnel_departure || style==LightingStyle::tunnel_departure_night)) {
            // The departure scene's throat is Z=0, with the tube along +Z.
            // Fade out the interior lighting at the real opening. Do not fog
            // an exterior ship or Earth merely because it is far from the eye.
            const auto eye_z=vng::dsl::max(lights.get(Eye{}).z(),0.F);
            // Exterior geometry is veiled by the depth-tested portal pass,
            // including materials (Earth) that do not opt into tunnel lighting.
            // Do not double-fog the terminal behind that veil.
            optical_depth=optical_depth*vng::dsl::select(world.z()>1.F,1.F,0.F)*vng::dsl::clamp(eye_z/4.F,0.F,1.F);
            const auto eye=lights.get(Eye{});
            const auto center_y=-eye_z*eye_z/(6371.F+vng::dsl::sqrt(vng::dsl::max(6371.F*6371.F-eye_z*eye_z,.001F)));
            const auto radial2=eye.x()*eye.x()+(eye.y()-center_y)*(eye.y()-center_y);
            optical_depth=optical_depth*vng::dsl::select(radial2<s.constant(example::earth::cinematic_tunnel_radius_km*example::earth::cinematic_tunnel_radius_km),1.F,0.F)*vng::dsl::select(eye_z<1000.F,1.F,0.F);
        }
        // Match the mouth veil's falloff, avoiding a bright flat disk against
        // walls that were using a different distance-to-transmission curve.
        const auto transmission=(style==LightingStyle::tunnel_departure || style==LightingStyle::tunnel_departure_night)
            ? vng::dsl::exp(-optical_depth) : 1.F/(1.F+optical_depth);
        hdr=hdr*transmission+s.constant(vng::Vec3{4.2F,4.05F,3.7F})*(1.F-transmission);
    }
    const auto radiance = vng::dsl::vec4(hdr, 1.0F);
    s.observe(SurfaceColor{}, radiance);
    s.observe(WorldPosition{}, world);
    s.observe(WorldNormal{}, normal);
    return s.output(vng::dsl::field<vng::shader::Color<0>>(radiance));
}

inline auto shader_program(LightingStyle style=LightingStyle::standard) {
    using VI = vng::shader::VertexInputs<Position, Color, Normal, Emission>;
    using VO = vng::shader::VertexOutputs<vng::shader::ClipPosition, vng::shader::smooth<Color>,
        vng::shader::smooth<WorldPosition>, vng::shader::smooth<WorldNormal>, vng::shader::smooth<Emission>>;
    using FI = vng::shader::FragmentInputs<vng::shader::smooth<Color>, vng::shader::smooth<WorldPosition>,
        vng::shader::smooth<WorldNormal>, vng::shader::smooth<Emission>>;
    using FO = vng::shader::FragmentOutputs<vng::shader::Color<0>>;
    auto vertex = vng::shader::vertex<VI, VO>("editor_mesh", [](auto& s, vng::dsl::Float4x4 model) {
        const auto world = model * vng::dsl::vec4(s.input(Position{}), 1.0F);
        const auto normal = transform_normal(model,s.input(Normal{}));
        return s.output(vng::dsl::field<vng::shader::ClipPosition>(s.camera().project(world.xyz())),
            vng::dsl::field<Color>(s.input(Color{})), vng::dsl::field<WorldPosition>(world.xyz()),
            vng::dsl::field<WorldNormal>(normal), vng::dsl::field<Emission>(s.input(Emission{})));
    });
    auto fragment = vng::shader::fragment<FI, FO>("editor_mesh", [style](auto& s, vng::dsl::Expr<Lighting> lights) {
        return shade(s, lights, style);
    });
    using Linked = vng::shader::Result<vng::shader::TypedGraphicsProgram<vng::Mat4, Lighting>>;
    if (!vertex) return Linked{std::unexpected(vertex.error())};
    if (!fragment) return Linked{std::unexpected(fragment.error())};
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

inline bool ignored_color(const vng::content::vmesh::Document& source) {
    const auto color = std::ranges::find(source.vertex_fields, "color/0", &vng::content::vmesh::VertexField::name);
    return color != source.vertex_fields.end() &&
        color->type != vng::content::vmesh::FieldType{vng::content::vmesh::ScalarType::Float32, 3} &&
        color->type != vng::content::vmesh::FieldType{vng::content::vmesh::ScalarType::Float32, 4};
}

inline vng::content::Result<Mesh> cpu_mesh(const vng::content::vmesh::Document& source) {
    auto schema = vng::content::vmesh::schema<Vertex>();
    schema.map("position", Position{});
    schema.map("color/0", Color{});
    auto surface = vng::content::vmesh::schema<Surface>();
    surface.map("normal", Normal{});
    surface.map("emission", Emission{});
    auto document = source;
    const auto color = std::ranges::find(document.vertex_fields, "color/0", &vng::content::vmesh::VertexField::name);
    if (color == document.vertex_fields.end()) {
        document.vertex_fields.push_back({"color/0", {vng::content::vmesh::ScalarType::Float32, 4},
            std::vector<vng::f32>(document.vertex_count * 4, 1)});
    } else if (color->type == vng::content::vmesh::FieldType{vng::content::vmesh::ScalarType::Float32, 3}) {
        const auto& rgb = std::get<std::vector<vng::f32>>(color->values);
        std::vector<vng::f32> rgba;
        rgba.reserve(document.vertex_count * 4);
        for (std::size_t vertex = 0; vertex < document.vertex_count; ++vertex)
            rgba.insert(rgba.end(), {rgb[vertex * 3], rgb[vertex * 3 + 1], rgb[vertex * 3 + 2], 1.F});
        color->type.components = 4;
        color->values = std::move(rgba);
    } else if (ignored_color(source)) {
        color->type = {vng::content::vmesh::ScalarType::Float32, 4};
        color->values = std::vector<vng::f32>(document.vertex_count * 4, 1);
    }
    // Optional preview conventions never rewrite the authored document. A
    // missing/unsupported normal keeps a mesh unlit; emission defaults to zero.
    for (const auto& [name, count] : {std::pair{"normal", 3U}, std::pair{"emission", 1U}}) {
        const vng::content::vmesh::FieldType type{vng::content::vmesh::ScalarType::Float32, static_cast<vng::u8>(count)};
        const auto field = std::ranges::find(document.vertex_fields, name, &vng::content::vmesh::VertexField::name);
        if (field == document.vertex_fields.end())
            document.vertex_fields.push_back({name, type, std::vector<vng::f32>(document.vertex_count * count, 0)});
        else if (field->type != type) {
            field->type = type;
            field->values = std::vector<vng::f32>(document.vertex_count * count, 0);
        }
    }
    return vng::content::vmesh::decode(document, schema, surface);
}
} // namespace editor_example::mesh_shading
