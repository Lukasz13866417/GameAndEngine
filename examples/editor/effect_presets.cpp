#include "effect_presets.hpp"
#include "import_format.hpp"
#include "animation.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace editor_example {
using namespace vng;
namespace {
bool valid_name(std::string_view name) {
    return !name.empty() && name.size()<=256 &&
        std::ranges::none_of(name,[](unsigned char c){return c<32 || c==127;});
}
content::Result<std::string> invalid(std::string message) {
    content::Diagnostic error;error.message=std::move(message);return std::unexpected(std::move(error));
}
}
bool valid_effect_settings(const SunSettings& s) {
    const auto range=[](f32 v,f32 lo,f32 hi){return std::isfinite(v)&&v>=lo&&v<=hi;};
    return range(s.radius,.1F,4)&&range(s.displacement,0,1)&&range(s.bloom,0,1);
}
SunSettings read_effect_settings(content::Reader r) {
    SunSettings defaults;
    return {r.get_or<f32>("radius",defaults.radius),r.get_or<f32>("displacement",defaults.displacement),
        r.get_or<f32>("bloom",defaults.bloom),r.get_or<bool>("white_spots",defaults.white_spots),
        r.get_or<bool>("visible",defaults.visible)};
}
void write_effect_settings(std::ostream& out,const SunSettings& s) {
    out<<"{ radius = "<<s.radius<<"; displacement = "<<s.displacement<<"; bloom = "<<s.bloom
       <<"; white_spots = "<<s.white_spots<<"; visible = "<<s.visible<<"; }";
}
content::Result<EffectPreset> read_effect_preset(const std::filesystem::path& path) {
    auto document=content::read_document(path,{.limits={.max_source_bytes=256*1024,.max_decoded_bytes=512*1024,
        .max_nodes=2048,.max_depth=16,.max_string_bytes=4096}});
    if(!document)return std::unexpected(document.error());
    return document->read([](content::Reader& r) {
        if(r.get<u32>("effect_preset")!=1)r.fail("Unsupported effect preset version");
        if(r.get<std::string>("type")!="sun")r.child("type").fail("Unsupported effect type (supported: sun)");
        EffectPreset preset{r.get<std::string>("name"),read_effect_settings(r.child("settings"))};
        if(!valid_name(preset.name))r.child("name").fail("Effect name must contain 1..256 bytes without control characters");
        if(!valid_effect_settings(preset.settings))r.child("settings").fail("Effect settings exceed editor limits");
        return preset;
    });
}
content::Result<std::string> encode_effect_preset(const EffectPreset& preset) {
    if(!valid_name(preset.name)||!valid_effect_settings(preset.settings))return invalid("Invalid effect preset name or settings");
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::boolalpha<<std::setprecision(9);
    out<<"vscene 1.0\neffect_preset = 1;\ntype = \"sun\";\nname = "<<std::quoted(preset.name)<<";\nsettings = ";
    write_effect_settings(out,preset.settings);out<<";\n";
    auto result=out.str();
    if(auto valid=content::parse_document(result);!valid)return std::unexpected(valid.error());
    return result;
}
content::Result<EffectPreset> effect_preset(const State& state,u32 id) {
    const auto* source=find_instance(state,id);
    if(!source || !effect_blueprint_settings(state,source->blueprint)) {
        content::Diagnostic error;error.message="Select an effect instance to export a preset";
        return std::unexpected(std::move(error));
    }
    const auto sampled=evaluate_instance(state,*source,state.viewport.time);
    return EffectPreset{source->name,std::get<SunSettings>(sampled.settings)};
}
content::Result<u32> import_asset(State& state,const std::filesystem::path& path) {
    const auto kind=import_kind(path);
    if(kind==ImportKind::mesh)return import_mesh(state,path);
    if(kind!=ImportKind::effect_preset) {
        content::Diagnostic error;error.message="Import supports .vmesh meshes and .veffect presets";
        return std::unexpected(std::move(error));
    }
    std::error_code error;
    if(path.empty() || path.native().find('\0')!=std::string::npos || !std::filesystem::is_regular_file(path,error)) {
        content::Diagnostic diagnostic;diagnostic.message="Effect import requires an existing regular .veffect file";
        return std::unexpected(std::move(diagnostic));
    }
    auto preset=read_effect_preset(path);if(!preset)return std::unexpected(preset.error());
    if(state.document.mesh_assets.size()+state.document.effect_assets.size()>=254 || state.document.next_blueprint_id<3 ||
       state.document.next_blueprint_id>=static_cast<u32>(BlueprintId::region)) {
        content::Diagnostic diagnostic;diagnostic.message="Imported blueprint limit reached";
        return std::unexpected(std::move(diagnostic));
    }
    auto candidate=state;
    const auto id=static_cast<BlueprintId>(candidate.document.next_blueprint_id++);
    candidate.document.effect_assets.push_back({id,std::move(preset->name),preset->settings});
    auto instance=instantiate(candidate,id);if(!instance)return std::unexpected(instance.error());
    // Validate full persistence/preview limits before changing either identity
    // counter or exposing a half-imported asset to the authoring session.
    if(auto valid=encode(candidate);!valid)return std::unexpected(valid.error());
    state=std::move(candidate);return *instance;
}
}
