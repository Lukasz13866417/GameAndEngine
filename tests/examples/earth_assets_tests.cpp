#include "support/earth_assets.hpp"
#include "support/earth_clouds.hpp"
#include "scenes/earth_scene.hpp"
#include "support/mesh_frame.hpp"
#include "support/earth_placement.hpp"
#include "support/earth_skyway.hpp"
#include "support/earth_connections.hpp"
#include "support/earth_structures.hpp"
#include "support/earth_infrastructure_placement.hpp"
#include "editor/editing_session.hpp"
#include "editor/animation.hpp"
#include "editor/preview_delivery_logic.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <chrono>
#include <iostream>
#include <limits>

TEST_CASE("Uniform tunnel supports include chosen ends with equal arc-length spacing", "[earth][scaffolds]") {
    namespace earth=example::earth;
    earth::InfrastructurePart part;part.kind=earth::InfrastructureKind::skyway;part.id=1;part.name="Test tunnel";
    part.location={0,0};part.end={25,12};
    const std::array parts{part};const auto curve=earth::TunnelCurve::create(part,parts,{});REQUIRE(curve);
    const earth::TunnelArc arc{*curve};
    const std::array endpoints{0.F,1.F};
    const auto spacing=arc.length()*.11F;
    const auto supports=earth::uniform_tunnel_supports(arc,.15F,.85F,spacing,endpoints);REQUIRE(supports);
    REQUIRE(supports->size()>4);CHECK((*supports)[0]==0);CHECK((*supports)[1]==1);
    CHECK((*supports)[2]==.15F);CHECK(supports->back()==.85F);
    const auto step=arc.distance((*supports)[3])-arc.distance((*supports)[2]);
    CHECK(step<=spacing);
    for(std::size_t i=3;i<supports->size();++i)
        CHECK(arc.distance((*supports)[i])-arc.distance((*supports)[i-1])==Catch::Approx(step).margin(.000001));
    CHECK_FALSE(earth::uniform_tunnel_supports(arc,.8F,.2F,spacing,endpoints));
    CHECK_FALSE(earth::uniform_tunnel_supports(arc,0,.8F,spacing,endpoints));
    CHECK_FALSE(earth::uniform_tunnel_supports(arc,.2F,.8F,0,endpoints));
    CHECK_FALSE(earth::uniform_tunnel_supports(arc,.2F,.8F,arc.length()/1000,endpoints));
    CHECK_FALSE(earth::uniform_tunnel_supports(arc,.2F,.8F,std::numeric_limits<float>::quiet_NaN(),endpoints));
    const std::array interior{0.F,.5F,1.F};
    CHECK_FALSE(earth::uniform_tunnel_supports(arc,.2F,.8F,spacing,interior));
}

namespace {
using namespace vng;
namespace earth=example::earth;
namespace project=editor_example;
static_assert(earth::max_earth_vertices<=editor::max_mesh_vertices);
const std::vector<f32>& values(const content::vmesh::Document& document,std::string_view name) {
    auto field=std::ranges::find(document.vertex_fields,name,&content::vmesh::VertexField::name);
    REQUIRE(field!=document.vertex_fields.end());return std::get<std::vector<f32>>(field->values);
}
Vec3 at(const std::vector<f32>& v,std::size_t i){return {v[i*3],v[i*3+1],v[i*3+2]};}
Vec3 sub(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
f32 dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 unit(Vec3 p){const auto length=std::sqrt(dot(p,p));return {p.x/length,p.y/length,p.z/length};}
}
TEST_CASE("Future Earth layers are deterministic independently switchable and preserve authored geometry", "[earth][infrastructure]") {
    auto source=earth::make_mesh({.visible=false});
    source.vertex_fields.push_back({"custom",{content::vmesh::ScalarType::UInt32,1},std::vector<u32>(source.vertex_count,17)});
    const earth::InfrastructureSettings all{true,true,true};
    auto future=earth::rebuild_infrastructure(source,all);REQUIRE(future);
    auto maximum=earth::rebuild_infrastructure(source,{true,true,true,4,2,3});REQUIRE(maximum);
    CHECK(maximum->vertex_count<earth::max_earth_vertices);
    REQUIRE(content::vmesh::validate(*future));CHECK(future->vertex_count<earth::max_earth_vertices);
    CHECK(*earth::infrastructure_settings(*future)==all);
    auto again=earth::rebuild_infrastructure(*future,all);REQUIRE(again);CHECK(*again==*future);
    for(const auto& original:source.vertex_fields) {
        const auto& actual=std::ranges::find(future->vertex_fields,original.name,&content::vmesh::VertexField::name)->values;
        std::visit([&](const auto& expected){const auto& v=std::get<std::decay_t<decltype(expected)>>(actual);CHECK(std::equal(expected.begin(),expected.end(),v.begin()));},original.values);
    }
    for(const auto option:std::array{earth::InfrastructureSettings{true,false,false},
            earth::InfrastructureSettings{false,true,false},earth::InfrastructureSettings{false,false,true}}) {
        auto isolated=earth::rebuild_infrastructure(*future,option);REQUIRE(isolated);
        const auto& owners=std::get<std::vector<u32>>(std::ranges::find(isolated->vertex_fields,"earth/infrastructure",&content::vmesh::VertexField::name)->values);
        const auto wanted=option.night_lights?1U:option.skyways?2U:3U;
        CHECK(std::ranges::count(owners,wanted)>0);
        CHECK(std::ranges::all_of(owners,[&](u32 id){return id==0||id==wanted;}));
    }
    auto disabled=earth::rebuild_infrastructure(*future,{});REQUIRE(disabled);
    CHECK(disabled->vertex_count==source.vertex_count);CHECK(disabled->faces==source.faces);
    for(const auto& f:source.vertex_fields)CHECK(*std::ranges::find(disabled->vertex_fields,f.name,&content::vmesh::VertexField::name)==f);
    auto savannah=earth::make_savannah_variant(*future);REQUIRE(savannah);
    const auto& c=values(*future,"color/0"),&changed=values(*savannah,"color/0");
    CHECK(std::equal(c.begin()+std::ptrdiff_t(source.vertex_count*4),c.end(),changed.begin()+std::ptrdiff_t(source.vertex_count*4)));
    auto clouds=earth::rebuild_clouds(*future,{.visible=false});REQUIRE(clouds);
    CHECK(values(*clouds,"position")==values(*future,"position"));
    // Reject hand-created bridges instead of silently destroying retained geometry.
    future->faces.emplace_back(0,1,u32(source.vertex_count));
    CHECK_FALSE(earth::rebuild_infrastructure(*future,{}));
    CHECK_FALSE(earth::rebuild_infrastructure(source,{.city_density=99}));
    source.metadata["earth/infrastructure/night_lights"]="maybe";
    CHECK_FALSE(earth::infrastructure_settings(source));
}

TEST_CASE("Tunnel classes share physical bores without changing route height or length", "[earth][infrastructure][tunnel-classes]") {
    earth::InfrastructurePart p{1,earth::InfrastructureKind::skyway,"Tunnel",{0,0},{9,0}};
    std::array parts{p};
    const auto original=earth::TunnelCurve::create(p,parts,{});REQUIRE(original);
    CHECK(original->size(.5F)==Catch::Approx(.6F)); // Exact 40% width reduction.
    for(const auto size:{earth::TunnelSizeClass::local,earth::TunnelSizeClass::regional,earth::TunnelSizeClass::trunk}) {
        p.tunnel_class=size;parts[0]=p;
        const auto curve=earth::TunnelCurve::create(p,parts,{});REQUIRE(curve);
        CHECK(2*earth::tunnel_inner_half_height*curve->size(.5F)*earth::reference_earth_radius_km==
            Catch::Approx(earth::tunnel_diameter_km(size)));
        CHECK(curve->sample(.5F).position==original->sample(.5F).position);
        CHECK(curve->sample(0).position==original->sample(0).position);
        CHECK(curve->sample(1).position==original->sample(1).position);
    }
    CHECK(earth::tunnel_diameter_km(earth::TunnelSizeClass::local)==6.F);
    p.tunnel_class=earth::TunnelSizeClass(99);CHECK_FALSE(earth::valid(p));
    auto source=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::skyway,{0,0});REQUIRE(source);
    auto recipe=earth::infrastructure_parts(*source)->front();recipe.tunnel_class=earth::TunnelSizeClass::local;
    auto edited=earth::edit_infrastructure(*source,recipe);REQUIRE(edited);
    CHECK(edited->faces==source->faces);CHECK(edited->vertex_count==source->vertex_count);
    auto bytes=content::vmesh::write_vmesh(*edited);REQUIRE(bytes);
    auto restored=content::vmesh::parse_vmesh(*bytes);REQUIRE(restored);
    CHECK(earth::infrastructure_parts(*restored)->front().tunnel_class==earth::TunnelSizeClass::local);
    auto legacy=*source;legacy.metadata["earth/infrastructure/parts-version"]="6";
    for(auto& [name,text]:legacy.metadata)if(name.starts_with("earth/infrastructure/part/"))for(unsigned i=0;i<4;++i)text.resize(text.rfind(' '));
    CHECK(earth::infrastructure_parts(legacy)->front().tunnel_class==earth::TunnelSizeClass::trunk);
}

TEST_CASE("Bezier tunnels retain controls, sample by de Casteljau and rebuild only their geometry", "[earth][bezier]") {
    auto base=earth::make_mesh({.visible=false});
    auto made=earth::add_infrastructure(base,earth::InfrastructureKind::skyway,{0,0});REQUIRE(made);
    auto p=earth::infrastructure_parts(*made)->front();
    auto initial=earth::TunnelCurve::create(p,std::span{&p,1},{});REQUIRE(initial);
    p.bezier_controls=initial->initial_bezier_controls();p.curve_segments=24;
    (*p.bezier_controls)[0].y+=.05F;
    auto curve=earth::TunnelCurve::create(p,std::span{&p,1},{});REQUIRE(curve);
    const auto a=curve->sample(0).position,b=curve->sample(1).position;
    using namespace earth::placement;
    const auto expected=add(mul(add(a,b),.125F),mul(add((*p.bezier_controls)[0],(*p.bezier_controls)[1]),.375F));
    const auto mid=curve->sample(.5F);
    CHECK(std::hypot(mid.position.x-expected.x,mid.position.y-expected.y,mid.position.z-expected.z)<1e-6F);
    CHECK(std::hypot(mid.tangent.x,mid.tangent.y,mid.tangent.z)==Catch::Approx(1));
    auto edited=earth::edit_infrastructure(*made,p);REQUIRE(edited);
    auto encoded=content::vmesh::write_vmesh(*edited);REQUIRE(encoded);
    auto decoded=content::vmesh::parse_vmesh(*encoded);REQUIRE(decoded);
    CHECK(earth::infrastructure_parts(*decoded)->front()==p);
    p.curve_segments=48;
    auto finer=earth::edit_infrastructure(*edited,p);REQUIRE(finer);
    CHECK(finer->vertex_count>edited->vertex_count);
    CHECK(std::equal(values(base,"position").begin(),values(base,"position").end(),values(*finer,"position").begin()));
    auto moved=earth::move_infrastructure(*finer,p.id,{25,12});REQUIRE(moved);
    auto moved_part=earth::infrastructure_parts(*moved)->front();REQUIRE(moved_part.bezier_controls);
    CHECK(*moved_part.bezier_controls!=*p.bezier_controls);
    CHECK(std::hypot((*moved_part.bezier_controls)[0].x,(*moved_part.bezier_controls)[0].y,(*moved_part.bezier_controls)[0].z)==
        Catch::Approx(std::hypot((*p.bezier_controls)[0].x,(*p.bezier_controls)[0].y,(*p.bezier_controls)[0].z)));
    p.curve_segments=257;CHECK_FALSE(earth::valid(p));p.curve_segments=48;
    p.bezier_controls->resize(15,{0,1,0});CHECK_FALSE(earth::valid(p));
}

TEST_CASE("Authored Earth network adds local routes and keeps its other geometry", "[earth][infrastructure][tunnel-classes]") {
    const auto base=earth::make_mesh({.visible=false});
    auto future=earth::rebuild_infrastructure(base,{true,true,true});REQUIRE(future);
    std::vector<u32> tags(future->vertex_count);for(u32 i=0;i<tags.size();++i)tags[i]=i+1;
    future->vertex_fields.push_back({"author/test-tag",{content::vmesh::ScalarType::UInt32,1},std::move(tags)});
    const auto& groups=std::get<std::vector<u32>>(std::ranges::find(future->vertex_fields,"earth/infrastructure",&content::vmesh::VertexField::name)->values);
    auto& colors=std::get<std::vector<f32>>(std::ranges::find(future->vertex_fields,"color/0",&content::vmesh::VertexField::name)->values);
    for(std::size_t i=0;i<groups.size();++i)if(groups[i]==3)colors[i*4]=.123F; // Hand-painted launch hubs.
    const auto before=earth::infrastructure_parts(*future);REQUIRE(before);
    const auto updated=earth::author_tunnel_network(*future);REQUIRE(updated);
    const auto after=earth::infrastructure_parts(*updated);REQUIRE(after);
    const auto& updated_groups=std::get<std::vector<u32>>(std::ranges::find(updated->vertex_fields,"earth/infrastructure",&content::vmesh::VertexField::name)->values);
    const auto& kept_tags=std::get<std::vector<u32>>(std::ranges::find(updated->vertex_fields,"author/test-tag",&content::vmesh::VertexField::name)->values);
    const auto& updated_colors=values(*updated,"color/0");
    for(std::size_t i=0;i<updated_groups.size();++i)if(updated_groups[i]!=2) {
        REQUIRE(kept_tags[i]>0);const auto old=kept_tags[i]-1;
        CHECK(at(values(*updated,"position"),i)==at(values(*future,"position"),old));
        CHECK(updated_colors[i*4]==colors[old*4]);
    }
    CHECK(after->size()==before->size()+14);
    std::array<unsigned,3> counts{};
    for(const auto& p:*after)if(p.kind==earth::InfrastructureKind::skyway)++counts[std::size_t(p.tunnel_class)];
    for(auto count:counts)CHECK(count>0);
    for(const auto& p:*before) {
        const auto found=std::ranges::find(*after,p.id,&earth::InfrastructurePart::id);REQUIRE(found!=after->end());
        CHECK(found->location==p.location);CHECK(found->end==p.end);
        if(p.kind!=earth::InfrastructureKind::skyway)CHECK(*found==p);
    }
    for(const auto& field:base.vertex_fields) {
        const auto& current=std::ranges::find(updated->vertex_fields,field.name,&content::vmesh::VertexField::name)->values;
        std::visit([&](const auto& expected){const auto& values=std::get<std::decay_t<decltype(expected)>>(current);
            CHECK(std::equal(expected.begin(),expected.end(),values.begin()));},field.values);
    }
    const auto twice=earth::author_tunnel_network(*updated);REQUIRE(twice);CHECK(*twice==*updated);
}
TEST_CASE("Focused Earth redesign replaces addons but retains cities and natural geometry", "[earth][infrastructure][redesign]") {
    auto source=earth::rebuild_infrastructure(earth::make_mesh({.visible=false}),{true,true,true});REQUIRE(source);
    source->metadata["earth/infrastructure/skyways"]="false";
    source->metadata["earth/infrastructure/launch_hubs"]="false";
    const auto before=earth::infrastructure_parts(*source);REQUIRE(before);
    std::vector<u32> tags(source->vertex_count);for(u32 i=0;i<tags.size();++i)tags[i]=i+1;
    source->vertex_fields.push_back({"author/test-tag",{content::vmesh::ScalarType::UInt32,1},std::move(tags)});
    auto& colors=std::get<std::vector<f32>>(std::ranges::find(source->vertex_fields,"color/0",&content::vmesh::VertexField::name)->values);
    for(std::size_t i=0;i<colors.size();i+=4)colors[i]=.123F; // Also preserve hand-painted cities.
    const auto result=earth::redesign_infrastructure(*source);REQUIRE(result);
    const auto after=earth::infrastructure_parts(*result);REQUIRE(after);
    unsigned cities{},tunnels{},hubs{};
    for(const auto& p:*after) {
        cities+=p.kind==earth::InfrastructureKind::settlement;
        tunnels+=p.kind==earth::InfrastructureKind::skyway;hubs+=p.kind==earth::InfrastructureKind::hub;
        if(p.kind==earth::InfrastructureKind::skyway)CHECK(p.bezier_controls.has_value());
    }
    CHECK(tunnels==11);CHECK(hubs==5);CHECK(after->size()==cities+tunnels+hubs);
    const auto express=std::ranges::find(*after,std::string{"Arabian express / cinematic local"},&earth::InfrastructurePart::name);
    REQUIRE(express!=after->end());
    CHECK(express->end.x>50);CHECK(express->end.x<54);
    CHECK(express->end.y>20);CHECK(express->end.y<24);
    CHECK(express->terminal_incline==8);
    const auto settings=earth::infrastructure_settings(*result);REQUIRE(settings);
    const auto hero_route=earth::TunnelCurve::create(*express,*after,*settings);REQUIRE(hero_route);
    for(const auto& part:*after)if(part.name=="Arabian express / inbound lane"||part.name=="Arabian express / service lane") {
        const auto neighbor=earth::TunnelCurve::create(part,*after,*settings);REQUIRE(neighbor);
        for(unsigned i=0;i<=20;++i) {
            const auto a=hero_route->sample(f32(i)/20).position,b=neighbor->sample(f32(i)/20).position;
            CHECK(std::hypot(a.x-b.x,a.y-b.y,a.z-b.z)*6371>45.F);
        }
    }
    for(const auto& p:*before) {
        const auto found=std::ranges::find(*after,p.id,&earth::InfrastructurePart::id);
        if(p.kind==earth::InfrastructureKind::settlement){REQUIRE(found!=after->end());CHECK(*found==p);}
        else CHECK(found==after->end());
    }
    const auto& old_groups=std::get<std::vector<u32>>(std::ranges::find(source->vertex_fields,"earth/infrastructure",&content::vmesh::VertexField::name)->values);
    const auto& new_tags=std::get<std::vector<u32>>(std::ranges::find(result->vertex_fields,"author/test-tag",&content::vmesh::VertexField::name)->values);
    std::size_t retained{};
    for(std::size_t i=0;i<new_tags.size();++i)if(new_tags[i]) {
        const auto old=new_tags[i]-1;++retained;
        CHECK((old_groups[old]==0||old_groups[old]==1));
        CHECK(at(values(*source,"position"),old)==at(values(*result,"position"),i));
        CHECK(values(*result,"color/0")[i*4]==colors[old*4]);
    }
    CHECK(retained==std::ranges::count_if(old_groups,[](u32 group){return group<=1;}));
    CHECK(result->vertex_count<earth::max_earth_vertices);
    const auto repeated=earth::redesign_infrastructure(*result);REQUIRE(repeated);CHECK(*repeated==*result);
}
TEST_CASE("Global corridors cover every orbital direction without replacing authored geometry", "[earth][infrastructure][global-network]") {
    auto source=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::settlement,{16,40});REQUIRE(source);
    const auto before=earth::infrastructure_parts(*source);REQUIRE(before);
    auto expanded=earth::expand_global_infrastructure(*source);REQUIRE(expanded);
    const auto parts=earth::infrastructure_parts(*expanded);REQUIRE(parts);
    CHECK(std::ranges::count(*parts,earth::InfrastructureKind::skyway,&earth::InfrastructurePart::kind)==25);
    CHECK(std::ranges::count(*parts,earth::InfrastructureKind::hub,&earth::InfrastructurePart::kind)==13);
    CHECK(parts->front()==before->front());
    // Appending recipes must not regenerate land, clouds or hand-painted cities.
    for(const auto name:{"position","normal","color/0","emission"}) {
        const auto& original=values(*source,name);const auto& after=values(*expanded,name);
        REQUIRE(after.size()>=original.size());CHECK(std::equal(original.begin(),original.end(),after.begin()));
    }
    REQUIRE(expanded->faces.size()>=source->faces.size());
    CHECK(std::equal(source->faces.begin(),source->faces.end(),expanded->faces.begin()));
    const auto settings=earth::infrastructure_settings(*expanded);REQUIRE(settings);
    std::vector<Vec3> midpoints;
    for(const auto& part:*parts)if(part.kind==earth::InfrastructureKind::skyway) {
        const auto curve=earth::TunnelCurve::create(part,*parts,*settings);REQUIRE(curve);
        CHECK(earth::TunnelArc(*curve).length()*6371>1800);
        for(unsigned step=0;step<=24;++step) {
            const auto p=curve->sample(f32(step)/24).position;
            CHECK(std::hypot(p.x,p.y,p.z)>1.02F); // No buried mid-span chords.
        }
        midpoints.push_back(earth::placement::unit(curve->sample(.5F).position));
    }
    // Cover ocean-facing views and both poles, not just populous longitudes.
    for(int lat=-90;lat<=90;lat+=10)for(int lon=-180;lon<180;lon+=10) {
        CAPTURE(lat,lon);
        const auto eye=earth::placement::direction({f32(lon),f32(lat)});
        CHECK(std::ranges::any_of(midpoints,[&](Vec3 p){return earth::placement::dot(eye,p)>.55F;}));
    }
    auto repeated=earth::expand_global_infrastructure(*expanded);REQUIRE(repeated);CHECK(*repeated==*expanded);
    CHECK(expanded->vertex_count<earth::max_earth_vertices);
}

TEST_CASE("Addon baselines multiply part size preserve recipes and keep tunnel sockets attached", "[earth][infrastructure][addon-scales]") {
    const auto base=earth::make_mesh({.visible=false});
    auto made=earth::add_infrastructure(base,earth::InfrastructureKind::processor,{0,0});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::terminal,{25,15});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::skyway,{0,12});REQUIRE(made);
    made=earth::connect_infrastructure_endpoint(*made,3,true,{2,1});REQUIRE(made);
    const auto parts=*earth::infrastructure_parts(*made);
    auto settings=*earth::infrastructure_settings(*made);settings.processor_scale=2;
    auto grown=earth::rebuild_infrastructure(*made,settings);REQUIRE(grown);
    CHECK(grown->vertex_count==made->vertex_count);CHECK(grown->faces==made->faces);
    CHECK(*earth::infrastructure_parts(*grown)==parts);
    const auto& ids=std::get<std::vector<u32>>(std::ranges::find(grown->vertex_fields,earth::infrastructure_part_field,&content::vmesh::VertexField::name)->values);
    const auto& p=values(*made,"position"),&q=values(*grown,"position");
    const auto pivot=earth::placement::mul(earth::placement::direction(parts[0].location),earth::infrastructure_base_radius);
    for(std::size_t i=0;i<ids.size();++i) {
        const auto expected=ids[i]==1?earth::placement::add(pivot,earth::placement::mul(sub(at(p,i),pivot),2)):at(p,i);
        CHECK(std::sqrt(dot(sub(at(q,i),expected),sub(at(q,i),expected)))<.000003F);
    }
    settings.addon_scale=1.5F;settings.terminal_scale=1.4F;settings.tunnel_scale=.8F;
    auto connected=earth::rebuild_infrastructure(*grown,settings);REQUIRE(connected);
    CHECK(*earth::infrastructure_parts(*connected)==parts);
    const auto ports=earth::infrastructure_sockets(parts,settings);REQUIRE(ports.size()==1);
    const auto curve=earth::TunnelCurve::create(parts.back(),parts,settings);REQUIRE(curve);
    CHECK(curve->sample(1).position==ports[0].position);CHECK(curve->size(1)==ports[0].size);
    CHECK(ports[0].size==Catch::Approx(settings.structure_size*parts[1].size*parts[1].scale*1.5F*1.4F));
    CHECK(curve->size(0)==Catch::Approx(settings.structure_size*parts[2].size*parts[2].scale*1.5F*.8F*
        earth::tunnel_width_coefficient(parts[2].tunnel_class)));
    auto saved=content::vmesh::write_vmesh(*connected);REQUIRE(saved);
    auto restored=content::vmesh::parse_vmesh(*saved);REQUIRE(restored);
    CHECK(*earth::infrastructure_settings(*restored)==settings);CHECK(*earth::infrastructure_parts(*restored)==parts);
    auto bad=settings;bad.addon_scale=0;CHECK_FALSE(earth::rebuild_infrastructure(*connected,bad));
    bad=settings;bad.tunnel_scale=std::numeric_limits<f32>::quiet_NaN();CHECK_FALSE(earth::valid(bad));
    CHECK(earth::infrastructure_settings(base)->addon_scale==1); // Legacy missing metadata defaults to identity.
    CHECK(earth::infrastructure_settings(base)->processor_scale==1);
}
TEST_CASE("Future Earth respects baked authoring frames and migrates legacy cloud layers", "[earth][infrastructure]") {
    auto legacy=content::vmesh::read_vmesh(std::filesystem::path(VNG_EARTH_ASSETS)/"earth.vmesh");REQUIRE(legacy);
    Mat4 transform=Mat4::identity();transform[0]={0,0,-2,0};transform[2]={2,0,0,0};transform[3]={4,5,6,1};
    auto framed=example::mesh_frame::transformed(*legacy,transform);REQUIRE(framed);
    auto rebuilt=earth::rebuild_infrastructure(*framed,{true,true,true});REQUIRE(rebuilt);
    auto local=earth::rebuild_infrastructure(*legacy,{true,true,true});REQUIRE(local);
    auto maximum=earth::rebuild_infrastructure(*legacy,{true,true,true,4,2,3,2,3});
    INFO((maximum?"rebuilt":maximum.error().message));REQUIRE(maximum);
    CHECK(maximum->vertex_count<earth::max_earth_vertices);
    auto expected=example::mesh_frame::transformed(*local,transform);REQUIRE(expected);
    CHECK(rebuilt->vertex_fields==expected->vertex_fields);CHECK(rebuilt->faces==expected->faces);
    auto clouds=earth::rebuild_clouds(*rebuilt,{.visible=false});REQUIRE(clouds);
    const auto& owners=std::get<std::vector<u32>>(std::ranges::find(clouds->vertex_fields,"earth/infrastructure",&content::vmesh::VertexField::name)->values);
    CHECK(std::size_t(std::ranges::count_if(owners,[](u32 id){return id>0;}))==local->vertex_count-legacy->vertex_count);
}
TEST_CASE("Earth megastructures have dark physical ribs tall towers and independent size controls", "[earth][infrastructure]") {
    auto base=earth::make_mesh({.visible=false});
    auto built=earth::rebuild_infrastructure(base,{false,true,true});REQUIRE(built);
    const auto& p=values(*built,"position"),&c=values(*built,"color/0"),&e=values(*built,"emission");
    const auto& ids=std::get<std::vector<u32>>(std::ranges::find(built->vertex_fields,"earth/infrastructure",&content::vmesh::VertexField::name)->values);
    std::size_t dark_ribs{},shell{},lit_trim{},hub_lights{};f32 skyway_top{},hub_top{};
    for(std::size_t i=0;i<ids.size();++i) {
        const auto v=at(p,i);const auto radius=std::sqrt(dot(v,v));
        if(ids[i]==2) {
            skyway_top=std::max(skyway_top,radius);
            dark_ribs+=e[i]==0&&c[i*4+2]<.04F;
            shell+=e[i]==0&&c[i*4+2]>.05F;
            lit_trim+=e[i]>0;
        } else if(ids[i]==3) {hub_top=std::max(hub_top,radius);hub_lights+=e[i]>0;}
    }
    CHECK(dark_ribs>1000);CHECK(shell>1000);CHECK(lit_trim>100);CHECK(hub_lights>100);
    // Measure physical luminous area, not vertex counts: the new interior
    // lanes share a sample at every bend, but remain narrow strips. The outer
    // shell also occludes those interior lamps in an orbital view.
    double opaque_area{},luminous_area{};
    for(const auto& face:built->faces) {
        const auto a=face.vertices[0],b=face.vertices[1],d=face.vertices[2];
        if(ids[a]!=2||ids[b]!=2||ids[d]!=2)continue;
        const auto normal=cross(sub(at(p,b),at(p,a)),sub(at(p,d),at(p,a)));
        const auto area=std::sqrt(dot(normal,normal))*.5;
        (e[a]>0||e[b]>0||e[d]>0?luminous_area:opaque_area)+=area;
    }
    CHECK(opaque_area>luminous_area*3);CHECK(skyway_top>1.08F);CHECK(hub_top>1.11F);
    auto bigger=earth::rebuild_infrastructure(*built,{false,true,true,1,1,2,1.5F,2});REQUIRE(bigger);
    CHECK(bigger->vertex_count==built->vertex_count);CHECK(bigger->faces==built->faces);
    CHECK(values(*bigger,"position")!=p);
    auto saved=earth::infrastructure_settings(*bigger);REQUIRE(saved);
    CHECK(saved->structure_size==1.5F);CHECK(saved->hub_height==2.F);
    built->metadata.erase("earth/infrastructure/structure_size");built->metadata.erase("earth/infrastructure/hub_height");
    auto defaults=earth::infrastructure_settings(*built);REQUIRE(defaults);CHECK(defaults->structure_size==1.F);CHECK(defaults->hub_height==1.F);
    CHECK_FALSE(earth::rebuild_infrastructure(base,{false,true,true,1,1,1,0,1}));
    CHECK_FALSE(earth::rebuild_infrastructure(base,{false,true,true,1,1,1,1,4}));
}
TEST_CASE("Infrastructure part placement is local durable and preserves neighboring custom geometry", "[earth][infrastructure][parts]") {
    const auto base=earth::make_mesh({.visible=false});
    auto hub=earth::add_infrastructure(base,earth::InfrastructureKind::hub,{30,15});REQUIRE(hub);
    auto parts=earth::infrastructure_parts(*hub);REQUIRE(parts);REQUIRE(parts->size()==1);const auto id=parts->front().id;
    hub->vertex_fields.push_back({"custom",{content::vmesh::ScalarType::UInt32,1},std::vector<u32>(hub->vertex_count,42)});
    auto route=earth::add_infrastructure(*hub,earth::InfrastructureKind::skyway,{20,10});REQUIRE(route);
    const auto before=*route;
    const auto start=std::chrono::steady_clock::now();
    auto moved=earth::move_infrastructure(before,id,{-15,48});REQUIRE(moved);
    std::cout<<"Infrastructure rigid move: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" ms\n";
    CHECK(moved->faces==before.faces);CHECK(moved->vertex_count==before.vertex_count);
    const auto& owners=std::get<std::vector<u32>>(std::ranges::find(before.vertex_fields,earth::infrastructure_part_field,&content::vmesh::VertexField::name)->values);
    const auto& before_positions=values(before,"position"),&moved_positions=values(*moved,"position");
    bool untouched=true,changed=false;
    for(std::size_t i=0;i<before.vertex_count;++i)for(unsigned c=0;c<3;++c) {
        const auto a=before_positions[i*3+c],b=moved_positions[i*3+c];
        if(owners[i]==id)changed|=a!=b;else untouched&=a==b;
    }
    CHECK(untouched);CHECK(changed);
    CHECK(std::ranges::find(moved->vertex_fields,"custom",&content::vmesh::VertexField::name)->values==
        std::ranges::find(before.vertex_fields,"custom",&content::vmesh::VertexField::name)->values);
    auto turned=earth::rotate_infrastructure(*moved,id,41);REQUIRE(turned);
    auto recipe=earth::infrastructure_parts(*turned);REQUIRE(recipe);
    CHECK(std::abs(recipe->front().location.x+15)<.001F);CHECK(std::abs(recipe->front().location.y-48)<.001F);
    auto rebuilt=earth::rebuild_infrastructure(*turned,*earth::infrastructure_settings(*turned));REQUIRE(rebuilt);
    const auto& p=values(*turned,"position"),&r=values(*rebuilt,"position");REQUIRE(p.size()==r.size());
    f32 difference{};for(std::size_t i=0;i<p.size();++i)difference=std::max(difference,std::abs(p[i]-r[i]));CHECK(difference<.00001F);
    auto saved=content::vmesh::write_vmesh(*turned);REQUIRE(saved);auto loaded=content::vmesh::parse_vmesh(*saved);REQUIRE(loaded);
    CHECK(*earth::infrastructure_parts(*loaded)==*recipe);
    auto frame=Mat4::identity();frame[0][0]=2;frame[1][1]=.7F;frame[2][2]=1.4F;frame[3]={5,6,7,1};
    auto framed=example::mesh_frame::transformed(before,frame);REQUIRE(framed);
    auto placed=earth::move_infrastructure(*framed,id,{-15,48});REQUIRE(placed);
    auto expected=example::mesh_frame::transformed(*moved,frame);REQUIRE(expected);
    const auto& actual=values(*placed,"position"),&wanted=values(*expected,"position");difference=0;
    for(std::size_t i=0;i<actual.size();++i)difference=std::max(difference,std::abs(actual[i]-wanted[i]));
    CHECK(difference<.00001F);
    auto removed=earth::remove_infrastructure(*turned,id);REQUIRE(removed);
    auto added=earth::add_infrastructure(*removed,earth::InfrastructureKind::hub,{0,20});REQUIRE(added);
    auto newest=earth::infrastructure_parts(*added);REQUIRE(newest);CHECK(newest->back().id>recipe->back().id);
}
TEST_CASE("Skyway formula pins endpoints and gives stable frames across the seam and poles", "[earth][infrastructure][skyway]") {
    const auto route=GENERATE(std::array<Vec2,2>{{{0,0},{80,0}}},std::array<Vec2,2>{{{179,20},{-165,30}}},
        std::array<Vec2,2>{{{0,89},{150,88}}},std::array<Vec2,2>{{{12,-30},{12.2F,-30}}});
    const auto rise=GENERATE(0.F,.03F,.3F);
    auto curve=earth::SkywayCurve::create(route[0],route[1],rise);REQUIRE(curve);
    auto reverse=earth::SkywayCurve::create(route[1],route[0],rise);REQUIRE(reverse);
    const auto a=earth::placement::unit(earth::placement::direction(route[0]));
    const auto b=earth::placement::unit(earth::placement::direction(route[1]));
    for(unsigned i=0;i<=24;++i) {
        const auto t=f32(i)/24;const auto sample=curve->sample(t),back=reverse->sample(1-t);
        const auto radius=std::hypot(sample.position.x,sample.position.y,sample.position.z);
        CHECK(std::abs(radius-(earth::skyway_endpoint_radius+rise*std::sin(std::numbers::pi_v<f32>*t)))<.00001F);
        CHECK(dot(sample.up,sample.position)>0);CHECK(std::abs(dot(sample.tangent,sample.up))<.00001F);
        CHECK(std::abs(dot(sample.tangent,sample.side))<.00001F);CHECK(std::abs(dot(sample.side,sample.up))<.00001F);
        for(auto v:{sample.tangent,sample.side,sample.up})CHECK(std::abs(dot(v,v)-1)<.00001F);
        for(unsigned c=0;c<3;++c) {
            CHECK(std::abs(sample.position[c]-back.position[c])<.00001F);
            CHECK(std::abs(sample.tangent[c]+back.tangent[c])<.0001F);
            if(i==0||i==24)CHECK(std::abs(sample.position[c]-(i==0?a[c]:b[c])*earth::skyway_endpoint_radius)<.000001F);
        }
    }
    CHECK_FALSE(earth::SkywayCurve::create({0,0},{0,0}));
    CHECK_FALSE(earth::SkywayCurve::create({0,0},{180,0}));
    CHECK_FALSE(earth::SkywayCurve::create({0,0},{150.1F,0}));
    CHECK_FALSE(earth::SkywayCurve::create({0,91},{20,0}));
    CHECK_FALSE(earth::SkywayCurve::create({0,0},{20,0},-1));
}
TEST_CASE("A tunnel endpoint patches only that tunnel and survives hide rebuild and delete", "[earth][infrastructure][parts]") {
    auto base=earth::make_mesh({.visible=false});
    auto generated=earth::rebuild_infrastructure(base,{false,true,true});REQUIRE(generated);
    auto parts=earth::infrastructure_parts(*generated);REQUIRE(parts);
    const auto tunnel=*std::ranges::find(*parts,earth::InfrastructureKind::skyway,&earth::InfrastructurePart::kind);
    auto moved=earth::move_infrastructure_endpoint(*generated,tunnel.id,true,{tunnel.end.x+3,tunnel.end.y+2});REQUIRE(moved);
    CHECK(moved->faces==generated->faces);CHECK(moved->vertex_count==generated->vertex_count);
    const auto& ids=std::get<std::vector<u32>>(std::ranges::find(moved->vertex_fields,earth::infrastructure_part_field,&content::vmesh::VertexField::name)->values);
    const auto& a=values(*generated,"position"),&b=values(*moved,"position");bool unchanged=true;
    for(std::size_t i=0;i<ids.size();++i)if(ids[i]!=tunnel.id)for(unsigned c=0;c<3;++c)unchanged&=a[i*3+c]==b[i*3+c];
    CHECK(unchanged);CHECK(a!=b);
    CHECK_FALSE(earth::move_infrastructure_endpoint(*generated,tunnel.id,true,tunnel.location));
    CHECK_FALSE(earth::move_infrastructure(*generated,tunnel.id,{0,100}));
    auto hidden=tunnel;hidden.visible=false;auto hide=earth::edit_infrastructure(*generated,hidden);REQUIRE(hide);
    auto rebuilt=earth::rebuild_infrastructure(*hide,*earth::infrastructure_settings(*hide));REQUIRE(rebuilt);
    CHECK(rebuilt->vertex_count==hide->vertex_count);
    auto show=earth::edit_infrastructure(*hide,tunnel);REQUIRE(show);CHECK(show->vertex_count==generated->vertex_count);
    auto removed=earth::remove_infrastructure(*generated,tunnel.id);REQUIRE(removed);
    auto again=earth::rebuild_infrastructure(*removed,*earth::infrastructure_settings(*removed));REQUIRE(again);
    auto recipes=earth::infrastructure_parts(*again);REQUIRE(recipes);
    CHECK(std::ranges::find(*recipes,tunnel.id,&earth::InfrastructurePart::id)==recipes->end());
    generated->faces.emplace_back(0,1,u32(base.vertex_count));CHECK_FALSE(earth::move_infrastructure(*generated,tunnel.id,{0,20}));
}
TEST_CASE("Part recipes retain surface orientation and reject malformed ownership", "[earth][infrastructure][parts]") {
    const auto kind=GENERATE(earth::InfrastructureKind::settlement,earth::InfrastructureKind::skyway,
        earth::InfrastructureKind::terminal,earth::InfrastructureKind::elevator,earth::InfrastructureKind::joiner,earth::InfrastructureKind::processor);
    const auto base=earth::make_mesh({.visible=false});
    auto created=earth::add_infrastructure(base,kind,{-70,30});REQUIRE(created);
    auto moved=earth::move_infrastructure(*created,1,{160,-40});REQUIRE(moved);
    auto turned=earth::rotate_infrastructure(*moved,1,123);REQUIRE(turned);
    auto rebuilt=earth::rebuild_infrastructure(*turned,*earth::infrastructure_settings(*turned));REQUIRE(rebuilt);
    const auto& a=values(*turned,"position"),&b=values(*rebuilt,"position");REQUIRE(a.size()==b.size());
    f32 difference{};
    for(std::size_t i=0;i<a.size();++i)difference=std::max(difference,std::abs(a[i]-b[i]));
    CHECK(difference<.00002F);
    auto corrupt=*turned;corrupt.metadata.erase("earth/infrastructure/next-id");
    CHECK_FALSE(earth::add_infrastructure(corrupt,kind));
    corrupt=*turned;corrupt.metadata.erase("earth/infrastructure/parts-version");
    CHECK_FALSE(earth::add_infrastructure(corrupt,kind));
    corrupt=*turned;corrupt.metadata["earth/infrastructure/part/1"]="invalid";
    CHECK_FALSE(earth::infrastructure_parts(corrupt));
    corrupt=*turned;
    std::ranges::find(corrupt.vertex_fields,earth::infrastructure_part_field,&content::vmesh::VertexField::name)->values=std::vector<f32>(corrupt.vertex_count);
    CHECK_FALSE(earth::infrastructure_parts(corrupt));
}
TEST_CASE("Terminals and elevators are scoped editable and persistent mesh parts", "[earth][infrastructure][structures]") {
    const auto kind=GENERATE(earth::InfrastructureKind::terminal,earth::InfrastructureKind::elevator,earth::InfrastructureKind::joiner,earth::InfrastructureKind::processor);
    auto base=earth::make_mesh({.visible=false});
    auto made=earth::add_infrastructure(base,kind,{35,20});REQUIRE(made);
    REQUIRE(content::vmesh::validate(*made));
    CHECK(earth::infrastructure_enabled(kind,*earth::infrastructure_settings(*made)));
    CHECK(made->vertex_count>base.vertex_count+400);CHECK(made->vertex_count<base.vertex_count+6000);
    auto recipes=earth::infrastructure_parts(*made);REQUIRE(recipes);REQUIRE(recipes->size()==1);
    auto part=recipes->front();CHECK(part.kind==kind);
    const auto& p=values(*made,"position"),&n=values(*made,"normal");
    for(std::size_t i=base.vertex_count;i<made->vertex_count;++i) {
        CHECK(std::abs(dot(at(n,i),at(n,i))-1)<.00001F);
        CHECK(std::isfinite(dot(at(p,i),at(p,i))));
    }
    for(std::size_t i=base.faces.size();i<made->faces.size();++i) {
        const auto face=made->faces[i];const auto a=at(p,face[0]),b=at(p,face[1]),c=at(p,face[2]);
        CHECK(dot(cross(sub(b,a),sub(c,a)),at(n,face[0]))>0.F);
    }
    part.size=1.4F;part.height=1.7F;
    auto edited=earth::edit_infrastructure(*made,part);INFO((edited?"ready":edited.error().message));REQUIRE(edited);
    CHECK(edited->faces==made->faces);CHECK(edited->vertex_count==made->vertex_count);
    CHECK(values(*edited,"position")!=p);
    const auto& before=values(base,"position"),&after=values(*edited,"position");
    CHECK(std::equal(before.begin(),before.end(),after.begin()));
    auto encoded=content::vmesh::write_vmesh(*edited);REQUIRE(encoded);
    auto decoded=content::vmesh::parse_vmesh(*encoded);REQUIRE(decoded);
    CHECK(earth::infrastructure_parts(*decoded)->front()==part);
    auto off=*earth::infrastructure_settings(*edited);off.large_structures=false;off.processors=false;
    auto hidden=earth::rebuild_infrastructure(*edited,off);REQUIRE(hidden);CHECK(hidden->vertex_count==base.vertex_count);
    CHECK(earth::infrastructure_parts(*hidden)->front()==part);
    auto removed=earth::remove_infrastructure(*edited,part.id);REQUIRE(removed);CHECK(removed->vertex_count==base.vertex_count);
}
TEST_CASE("Tunnel terminals follow endpoint edits and old recipes load without terminals", "[earth][infrastructure][structures]") {
    auto base=earth::make_mesh({.visible=false});
    auto plain=earth::add_infrastructure(base,earth::InfrastructureKind::skyway,{10,25});REQUIRE(plain);
    auto part=earth::infrastructure_parts(*plain)->front();part.terminal_a=part.terminal_b=true;
    auto terminal=earth::edit_infrastructure(*plain,part);REQUIRE(terminal);
    CHECK(terminal->vertex_count>plain->vertex_count+1000);
    auto moved=earth::move_infrastructure_endpoint(*terminal,part.id,true,{35,15});REQUIRE(moved);
    CHECK(moved->vertex_count==terminal->vertex_count);CHECK(moved->faces==terminal->faces);
    const auto recipe=earth::infrastructure_parts(*moved)->front();
    CHECK(recipe.terminal_a);CHECK(recipe.terminal_b);CHECK(recipe.location==part.location);CHECK(recipe.end==Vec2{35,15});
    const auto& a=values(*terminal,"position"),&b=values(*moved,"position");
    CHECK(a!=b);CHECK(std::equal(a.begin(),a.begin()+std::ptrdiff_t(base.vertex_count*3),b.begin()));
    auto saved=content::vmesh::write_vmesh(*moved);REQUIRE(saved);auto restored=content::vmesh::parse_vmesh(*saved);REQUIRE(restored);
    CHECK(earth::infrastructure_parts(*restored)->front()==recipe);
    auto old=*plain;old.metadata["earth/infrastructure/parts-version"]="1";
    auto& text=old.metadata["earth/infrastructure/part/1"];text.resize(text.rfind('"')+1);
    auto legacy=earth::infrastructure_parts(old);REQUIRE(legacy);CHECK_FALSE(legacy->front().terminal_a);CHECK_FALSE(legacy->front().terminal_b);
    auto upgraded=earth::edit_infrastructure(old,part);REQUIRE(upgraded);
    CHECK(upgraded->metadata.at("earth/infrastructure/parts-version")=="9");
    CHECK(earth::infrastructure_parts(*upgraded)->front().terminal_b);
    // v8 did not store inclination; keep its 30-degree geometry on load.
    auto v8=*terminal;v8.metadata["earth/infrastructure/parts-version"]="8";
    auto& old_text=v8.metadata["earth/infrastructure/part/1"];old_text.resize(old_text.rfind(' '));
    auto old_parts=earth::infrastructure_parts(v8);REQUIRE(old_parts);CHECK(old_parts->front().terminal_incline==30);
    part.terminal_incline=8;
    auto shallow=earth::edit_infrastructure(*terminal,part);REQUIRE(shallow);
    CHECK(shallow->vertex_count==terminal->vertex_count);
    CHECK(values(*shallow,"position")!=values(*terminal,"position"));
    const auto encoded=content::vmesh::write_vmesh(*shallow);REQUIRE(encoded);
    const auto decoded=content::vmesh::parse_vmesh(*encoded);REQUIRE(decoded);
    CHECK(earth::infrastructure_parts(*decoded)->front()==part);
    auto invalid=part;invalid.terminal_incline=61;CHECK_FALSE(earth::edit_infrastructure(*shallow,invalid));
    invalid.terminal_incline=std::numeric_limits<f32>::quiet_NaN();CHECK_FALSE(earth::valid(invalid));
    auto corrupt=*terminal;corrupt.metadata["earth/infrastructure/part/1"]+=" junk";CHECK_FALSE(earth::infrastructure_parts(corrupt));
    part.kind=earth::InfrastructureKind::elevator;CHECK_FALSE(earth::valid(part));
}
TEST_CASE("Named tunnel sockets follow structures and regenerate only their dependents", "[earth][infrastructure][sockets]") {
    auto base=earth::make_mesh({.visible=false});
    auto made=earth::add_infrastructure(base,earth::InfrastructureKind::terminal,{8,-15});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::joiner,{0,0});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::skyway,{0,12});REQUIRE(made);
    made=earth::connect_infrastructure_endpoint(*made,3,true,{2,1});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::skyway,{0,-5});REQUIRE(made);
    made=earth::connect_infrastructure_endpoint(*made,4,false,{2,3});REQUIRE(made);
    made=earth::connect_infrastructure_endpoint(*made,4,true,{1,1});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::skyway,{-12,-14});REQUIRE(made);
    made=earth::connect_infrastructure_endpoint(*made,5,true,{2,2});REQUIRE(made);
    const auto parts=earth::infrastructure_parts(*made);REQUIRE(parts);
    const auto settings=*earth::infrastructure_settings(*made);
    const auto sockets=earth::infrastructure_sockets(*parts,settings);REQUIRE(sockets.size()==4);
    for(const auto& part:*parts)if(part.kind==earth::InfrastructureKind::skyway) {
        auto curve=earth::TunnelCurve::create(part,*parts,settings);REQUIRE(curve);
        for(bool end:{false,true}) {
            const auto ref=end?part.socket_b:part.socket_a;if(!ref)continue;
            const auto socket=std::ranges::find(sockets,ref,&earth::TunnelSocket::id);REQUIRE(socket!=sockets.end());
            const auto sample=curve->sample(end?1:0);
            CHECK(sample.position==socket->position);
            CHECK(dot(sample.tangent,socket->outward)==Catch::Approx(end?-1.F:1.F).margin(.00001F));
            CHECK(curve->size(end?1:0)==socket->size);
            CHECK(std::sqrt(dot(sample.position,sample.position))>earth::skyway_endpoint_radius);
        }
        for(unsigned i=0;i<=100;++i) {
            const auto s=curve->sample(f32(i)/100);
            CHECK(dot(s.position,s.position)>=1.018F*1.018F-.00001F);
            CHECK(dot(s.tangent,s.tangent)==Catch::Approx(1).margin(.00001F));
            CHECK(dot(s.side,s.up)==Catch::Approx(0).margin(.00001F));
        }
    }
    CHECK_FALSE(earth::connect_infrastructure_endpoint(*made,3,false,{2,2})); // occupied
    CHECK_FALSE(earth::connect_infrastructure_endpoint(*made,3,false,{99,1}));
    CHECK_FALSE(earth::connect_infrastructure_endpoint(*made,3,false,{1,3}));
    CHECK_FALSE(earth::connect_infrastructure_endpoint(*made,3,false,{3,1})); // no tunnel-to-tunnel edges
    CHECK_FALSE(earth::move_infrastructure_endpoint(*made,3,true,{30,0})); // cannot silently detach
    CHECK_FALSE(earth::move_infrastructure(*made,3,{30,0}));
    const auto owner=std::ranges::find(made->vertex_fields,earth::infrastructure_part_field,&content::vmesh::VertexField::name);
    const auto& ids=std::get<std::vector<u32>>(owner->values);
    for(bool property:{false,true}) {
        auto part=(*parts)[1];part.size=1.4F;part.height=1.8F;
        auto changed=property?earth::edit_infrastructure(*made,part):earth::move_infrastructure(*made,2,{2,1});REQUIRE(changed);
        CHECK(changed->faces==made->faces);CHECK(changed->vertex_count==made->vertex_count);
        const auto& before=values(*made,"position"),&after=values(*changed,"position");
        std::array<std::size_t,6> count{};
        for(std::size_t i=0;i<ids.size();++i)count[ids[i]]+=at(before,i)!=at(after,i);
        CHECK(count[0]==0);CHECK(count[1]==0); // terrain and unrelated terminal exactly preserved
        for(unsigned id:{2U,3U,4U,5U})CHECK(count[id]>0);
    }
    auto turned=earth::rotate_infrastructure(*made,2,25);REQUIRE(turned);
    auto bytes=content::vmesh::write_vmesh(*turned);REQUIRE(bytes);
    auto loaded=content::vmesh::parse_vmesh(*bytes);REQUIRE(loaded);
    const auto loaded_parts=earth::infrastructure_parts(*loaded),turned_parts=earth::infrastructure_parts(*turned);
    REQUIRE(loaded_parts);REQUIRE(turned_parts);CHECK(*loaded_parts==*turned_parts);
    auto rebuilt=earth::rebuild_infrastructure(*loaded,settings);REQUIRE(rebuilt);
    REQUIRE(rebuilt->vertex_count==turned->vertex_count);
    const auto& a=values(*turned,"position"),&b=values(*rebuilt,"position");
    for(std::size_t i=0;i<a.size();++i)CHECK(std::abs(a[i]-b[i])<.00002F);
    auto detached=earth::connect_infrastructure_endpoint(*made,4,true,{});REQUIRE(detached);
    auto detached_parts=earth::infrastructure_parts(*detached);REQUIRE(detached_parts);
    CHECK_FALSE((*detached_parts)[3].socket_b);CHECK((*detached_parts)[3].socket_a==earth::TunnelSocketRef{2,3});
    const auto old_end=earth::TunnelCurve::create((*parts)[3],*parts,settings)->sample(1).position;
    const auto free_end=earth::TunnelCurve::create((*detached_parts)[3],*detached_parts,settings)->sample(1).position;
    CHECK(std::sqrt(dot(sub(old_end,free_end),sub(old_end,free_end)))<.00001F);
    CHECK((*detached_parts)[3].altitude_b>0);
    auto removed=earth::remove_infrastructure(*made,2);REQUIRE(removed);
    auto remaining=earth::infrastructure_parts(*removed);REQUIRE(remaining);CHECK(remaining->size()==4);
    for(const auto& part:*remaining){CHECK(part.socket_a.part!=2);CHECK(part.socket_b.part!=2);}
    CHECK((*remaining)[2].socket_b==earth::TunnelSocketRef{1,1}); // other attachment survives
    // Parts and references are still canonical when the whole blueprint has a baked authoring frame.
    auto frame=Mat4::identity();frame[0]={0,0,-1.3F,0};frame[1]={0,.8F,0,0};frame[2]={1.1F,0,0,0};frame[3]={2,3,4,1};
    auto baked=example::mesh_frame::transformed(*made,frame);REQUIRE(baked);
    auto moved=earth::move_infrastructure(*baked,2,{2,1});REQUIRE(moved);
    auto canonical=earth::move_infrastructure(*made,2,{2,1});REQUIRE(canonical);
    auto expected=example::mesh_frame::transformed(*canonical,frame);REQUIRE(expected);
    const auto& actual_p=values(*moved,"position"),&expected_p=values(*expected,"position");
    for(std::size_t i=0;i<actual_p.size();++i)CHECK(std::abs(actual_p[i]-expected_p[i])<.00002F);
}
TEST_CASE("Version two structures load without connections and can attach on demand", "[earth][infrastructure][sockets]") {
    auto made=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::terminal,{0,0});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::skyway,{0,12});REQUIRE(made);
    made->metadata["earth/infrastructure/parts-version"]="2";
    for(auto& [key,text]:made->metadata)if(key.starts_with("earth/infrastructure/part/"))text.resize(text.rfind('"')+5);
    auto parts=earth::infrastructure_parts(*made);REQUIRE(parts);CHECK_FALSE(parts->back().socket_a);
    auto attached=earth::connect_infrastructure_endpoint(*made,2,true,{1,1});REQUIRE(attached);
    CHECK(attached->metadata.at("earth/infrastructure/parts-version")=="9");
    CHECK(earth::infrastructure_parts(*attached)->back().socket_b==earth::TunnelSocketRef{1,1});
}
TEST_CASE("Free tunnel ends retain altitude with sparse edits and backward-compatible recipes", "[earth][infrastructure][altitude]") {
    auto base=earth::make_mesh({.visible=false});
    auto made=earth::add_infrastructure(base,earth::InfrastructureKind::skyway,{0,0});REQUIRE(made);
    auto old=*made;old.metadata["earth/infrastructure/parts-version"]="3";
    auto& text=old.metadata["earth/infrastructure/part/1"];
    for(unsigned i=0;i<11;++i)text.resize(text.rfind(' ')); // v3 predates heights, supports, size classes and incline
    auto parts=earth::infrastructure_parts(old);REQUIRE(parts);CHECK(parts->front().altitude_a==0);
    const auto elevated=earth::place_infrastructure_endpoint(old,1,false,{0,0,1.2F});REQUIRE(elevated);
    parts=earth::infrastructure_parts(*elevated);REQUIRE(parts);
    CHECK(parts->front().altitude_a==Catch::Approx(1.2F-earth::skyway_endpoint_radius));
    CHECK(parts->front().altitude_b==0);
    const auto curve=earth::TunnelCurve::create(parts->front(),*parts,*earth::infrastructure_settings(*elevated));REQUIRE(curve);
    CHECK(curve->sample(0).position.z==Catch::Approx(1.2F));
    CHECK(elevated->vertex_count>made->vertex_count); // grounded scaffolds below raised end
    const auto higher=earth::place_infrastructure_endpoint(*elevated,1,false,{0,0,1.3F});REQUIRE(higher);
    CHECK(higher->faces==elevated->faces);CHECK(higher->vertex_count==elevated->vertex_count);
    const auto& p=values(*higher,"position");const auto& terrain=values(base,"position");
    CHECK(std::equal(terrain.begin(),terrain.end(),p.begin()));
    const auto lateral=earth::move_infrastructure_endpoint(*higher,1,false,{10,8});REQUIRE(lateral);
    CHECK(earth::infrastructure_parts(*lateral)->front().altitude_a==earth::infrastructure_parts(*higher)->front().altitude_a);
    const auto encoded=content::vmesh::write_vmesh(*lateral);REQUIRE(encoded);
    const auto decoded=content::vmesh::parse_vmesh(*encoded);REQUIRE(decoded);
    CHECK(*earth::infrastructure_parts(*decoded)==*earth::infrastructure_parts(*lateral));
    CHECK_FALSE(earth::place_infrastructure_endpoint(*higher,1,false,{0,0,.8F}));
    CHECK_FALSE(earth::place_infrastructure_endpoint(*higher,1,false,{0,0,earth::skyway_endpoint_radius+earth::max_tunnel_endpoint_altitude+1}));
    CHECK_FALSE(earth::place_infrastructure_endpoint(*higher,1,false,{0,0,std::numeric_limits<float>::infinity()}));
}
TEST_CASE("Dispersal terminal incline increases without moving its attachment throat", "[earth][infrastructure][structures]") {
    const auto path=earth::detail::freestanding_terminal_path(1,1);
    CHECK(path.front().position.y==Catch::Approx(1.025F));
    CHECK(path.back().position.y-path.front().position.y==Catch::Approx(.026F));
    CHECK(path.back().tangent.y>path.front().tangent.y);
    const auto taller=earth::detail::freestanding_terminal_path(1,2);
    CHECK(taller.back().position.y-taller.front().position.y==Catch::Approx(.052F));
}
TEST_CASE("Infrastructure structures lift and scale uniformly with independently grounded supports", "[earth][infrastructure][structure-placement]") {
    const auto kind=GENERATE(earth::InfrastructureKind::terminal,earth::InfrastructureKind::joiner,
        earth::InfrastructureKind::hub,earth::InfrastructureKind::elevator,earth::InfrastructureKind::processor);
    const auto base=earth::make_mesh({.visible=false});
    auto made=earth::add_infrastructure(base,kind,{25,15});REQUIRE(made);
    auto part=earth::infrastructure_parts(*made)->front();part.scaffold=false;
    auto bare=earth::edit_infrastructure(*made,part);REQUIRE(bare);
    auto altered=part;altered.scale=2;altered.altitude=.12F;
    auto grown=earth::edit_infrastructure(*bare,altered);REQUIRE(grown);
    CHECK(grown->vertex_count==bare->vertex_count);CHECK(grown->faces==bare->faces);
    const auto normal=earth::placement::direction(part.location),pivot=earth::placement::mul(normal,earth::infrastructure_base_radius);
    const auto& p=values(*bare,"position"),&q=values(*grown,"position"),&n=values(*grown,"normal");
    for(std::size_t i=base.vertex_count;i<bare->vertex_count;++i) {
        const auto expected=earth::placement::add(pivot,earth::placement::add(earth::placement::mul(sub(at(p,i),pivot),2),earth::placement::mul(normal,.12F)));
        CHECK(std::sqrt(dot(sub(at(q,i),expected),sub(at(q,i),expected)))<.000002F);
        CHECK(dot(at(n,i),at(n,i))==Catch::Approx(1).margin(.00001F));
    }
    altered.scaffold=true;auto supported=earth::edit_infrastructure(*grown,altered);REQUIRE(supported);
    CHECK(supported->vertex_count>grown->vertex_count);
    // Support feet stay at the surface even though the hull moves upward.
    auto minimum=10.F;const auto& supported_p=values(*supported,"position");
    for(std::size_t i=base.vertex_count;i<supported->vertex_count;++i)minimum=std::min(minimum,std::sqrt(dot(at(supported_p,i),at(supported_p,i))));
    CHECK(minimum<1.02F);
    const auto radius=earth::infrastructure_handle_base(altered,*earth::infrastructure_settings(*supported));
    const auto target=earth::placement::mul(normal,radius+.2F);
    auto higher=earth::place_infrastructure(*supported,part.id,target);REQUIRE(higher);
    CHECK(earth::infrastructure_parts(*higher)->front().altitude==Catch::Approx(.2F));
    CHECK(higher->faces==supported->faces); // further lifting is a sparse update
    auto moved=earth::move_infrastructure(*higher,part.id,{30,20});REQUIRE(moved);
    CHECK(earth::infrastructure_parts(*moved)->front().altitude==earth::infrastructure_parts(*higher)->front().altitude);
    auto rebuilt=earth::rebuild_infrastructure(*moved,*earth::infrastructure_settings(*moved));REQUIRE(rebuilt);
    const auto& a=values(*moved,"position"),&b=values(*rebuilt,"position");REQUIRE(a.size()==b.size());
    for(std::size_t i=0;i<a.size();++i)CHECK(std::abs(a[i]-b[i])<.00002F);
    const auto& terrain=values(base,"position");CHECK(std::equal(terrain.begin(),terrain.end(),a.begin()));
    auto encoded=content::vmesh::write_vmesh(*higher);REQUIRE(encoded);
    auto decoded=content::vmesh::parse_vmesh(*encoded);REQUIRE(decoded);
    CHECK(*earth::infrastructure_parts(*decoded)==*earth::infrastructure_parts(*higher));
}
TEST_CASE("Structure altitude and overall size move connected socket frames and only dependent tunnels", "[earth][infrastructure][structure-placement][sockets]") {
    auto made=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::terminal,{0,0});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::skyway,{0,12});REQUIRE(made);
    made=earth::connect_infrastructure_endpoint(*made,2,true,{1,1});REQUIRE(made);
    auto part=earth::infrastructure_parts(*made)->front();part.altitude=.15F;part.scale=1.8F;
    auto updated=earth::edit_infrastructure(*made,part);REQUIRE(updated);
    const auto parts=earth::infrastructure_parts(*updated);REQUIRE(parts);const auto settings=*earth::infrastructure_settings(*updated);
    const auto sockets=earth::infrastructure_sockets(*parts,settings);REQUIRE(sockets.size()==1);
    const auto route=earth::TunnelCurve::create(parts->back(),*parts,settings);REQUIRE(route);
    CHECK(route->sample(1).position==sockets[0].position);CHECK(route->size(1)==sockets[0].size);
    CHECK(sockets[0].size==Catch::Approx(settings.structure_size*part.size*part.scale));
    auto frame=Mat4::identity();frame[0][0]=1.5F;frame[1][1]=.8F;frame[2][2]=1.2F;frame[3]={2,3,4,1};
    const auto baked=example::mesh_frame::transformed(*made,frame);REQUIRE(baked);
    const auto edit_baked=earth::edit_infrastructure(*baked,part);REQUIRE(edit_baked);
    const auto expected=example::mesh_frame::transformed(*updated,frame);REQUIRE(expected);
    const auto& p=values(*edit_baked,"position"),&q=values(*expected,"position");REQUIRE(p.size()==q.size());
    for(std::size_t i=0;i<p.size();++i)CHECK(std::abs(p[i]-q[i])<.00002F);
    auto old=*made;old.metadata["earth/infrastructure/parts-version"]="4";
    for(auto& [key,text]:old.metadata)if(key.starts_with("earth/infrastructure/part/"))for(unsigned i=0;i<9;++i)text.resize(text.rfind(' '));
    const auto legacy=earth::infrastructure_parts(old);REQUIRE(legacy);
    CHECK(legacy->front().altitude==0);CHECK(legacy->front().scale==1);CHECK(legacy->front().scaffold);CHECK(legacy->front().scaffold_spacing==0);
}
TEST_CASE("Tunnel scaffolds are optional and spaced by arc distance with bounded density", "[earth][infrastructure][scaffolds]") {
    auto made=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::skyway,{0,0});REQUIRE(made);
    const auto original=earth::infrastructure_parts(*made)->front();auto part=original;part.scaffold=false;
    auto bare=earth::edit_infrastructure(*made,part);REQUIRE(bare);CHECK(bare->vertex_count<made->vertex_count);
    part.scaffold=true;part.scaffold_spacing=.06F;
    auto spaced=earth::edit_infrastructure(*bare,part);REQUIRE(spaced);CHECK(spaced->vertex_count>made->vertex_count);
    const auto parts=*earth::infrastructure_parts(*spaced);const auto settings=*earth::infrastructure_settings(*spaced);
    auto curve=earth::TunnelCurve::create(part,parts,settings);REQUIRE(curve);
    auto locations=curve->scaffold_parameters(part.scaffold_spacing);REQUIRE(locations);CHECK(locations->size()>2);
    CHECK(locations->front()>0);CHECK(locations->back()<1);
    for(std::size_t i=1;i<locations->size();++i) {
        const auto delta=sub(curve->sample((*locations)[i]).position,curve->sample((*locations)[i-1]).position);
        CHECK(std::sqrt(dot(delta,delta))<=part.scaffold_spacing*1.01F);
    }
    part.scaffold_spacing=.03F;auto denser=earth::edit_infrastructure(*spaced,part);REQUIRE(denser);
    CHECK(denser->vertex_count>spaced->vertex_count);
    part.scaffold_spacing=1e-8F;CHECK_FALSE(earth::edit_infrastructure(*spaced,part));
    part.scaffold_spacing=-1;CHECK_FALSE(earth::valid(part));
    part=original;part.scale=2;auto scaled=earth::edit_infrastructure(*made,part);REQUIRE(scaled);
    const auto scaled_parts=*earth::infrastructure_parts(*scaled);const auto larger=earth::TunnelCurve::create(part,scaled_parts,settings);REQUIRE(larger);
    CHECK(larger->sample(0).position==curve->sample(0).position);CHECK(larger->sample(1).position==curve->sample(1).position);
    CHECK(larger->size(.5F)==Catch::Approx(2*curve->size(.5F)));
}
TEST_CASE("Manual tunnel supports persist and only update their owning tunnel", "[earth][infrastructure][manual-scaffolds]") {
    auto made=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::skyway,{0,0});REQUIRE(made);
    made=earth::add_infrastructure(*made,earth::InfrastructureKind::hub,{40,20});REQUIRE(made);
    auto part=earth::infrastructure_parts(*made)->front();part.scaffold_positions=std::vector<f32>{.2F,.8F};
    auto manual=earth::edit_infrastructure(*made,part);REQUIRE(manual);
    const auto original=*manual;
    (*part.scaffold_positions)[0]=.9F; // crossing preserves handle order
    auto moved=earth::edit_infrastructure(*manual,part);REQUIRE(moved);
    CHECK(moved->faces==manual->faces);CHECK(moved->vertex_count==manual->vertex_count);
    const auto& owners=std::get<std::vector<u32>>(std::ranges::find(manual->vertex_fields,
        earth::infrastructure_part_field,&content::vmesh::VertexField::name)->values);
    const auto& a=values(*manual,"position"),&b=values(*moved,"position");CHECK(a!=b);
    for(std::size_t i=0;i<owners.size();++i)if(owners[i]!=part.id)
        for(unsigned c=0;c<3;++c)CHECK(a[i*3+c]==b[i*3+c]);
    CHECK(*manual==original);
    auto bytes=content::vmesh::write_vmesh(*moved);REQUIRE(bytes);
    auto loaded=content::vmesh::parse_vmesh(*bytes);REQUIRE(loaded);
    CHECK(earth::infrastructure_parts(*loaded)->front()==part);
    auto stretched=earth::move_infrastructure_endpoint(*moved,part.id,true,{25,10});REQUIRE(stretched);
    CHECK(earth::infrastructure_parts(*stretched)->front().scaffold_positions==part.scaffold_positions);
    CHECK(values(*stretched,"position")!=b);
    part.scaffold=false;auto hidden=earth::edit_infrastructure(*moved,part);REQUIRE(hidden);
    CHECK(earth::infrastructure_parts(*hidden)->front().scaffold_positions==part.scaffold_positions);
    part.scaffold=true;part.scaffold_positions->clear();auto empty=earth::edit_infrastructure(*hidden,part);REQUIRE(empty);
    CHECK(empty->vertex_fields==hidden->vertex_fields);CHECK(empty->faces==hidden->faces);
    part.scaffold_positions.reset();auto automatic=earth::edit_infrastructure(*empty,part);REQUIRE(automatic);
    CHECK(automatic->vertex_count==made->vertex_count);
    auto legacy=*made;legacy.metadata["earth/infrastructure/parts-version"]="5";
    for(auto& [key,text]:legacy.metadata)if(key.starts_with("earth/infrastructure/part/"))for(unsigned i=0;i<5;++i)text.resize(text.rfind(' '));
    auto parts=earth::infrastructure_parts(legacy);REQUIRE(parts);CHECK_FALSE(parts->front().scaffold_positions);
    part.scaffold_positions=std::vector<f32>(65,.5F);CHECK_FALSE(earth::edit_infrastructure(*made,part));
    part.scaffold_positions=std::vector<f32>{1.01F};CHECK_FALSE(earth::valid(part));
    part.scaffold_positions=std::vector<f32>{std::numeric_limits<f32>::quiet_NaN()};CHECK_FALSE(earth::valid(part));
    part.scaffold_positions=std::vector<f32>{.5F};part.kind=earth::InfrastructureKind::hub;CHECK_FALSE(earth::valid(part));
    auto corrupt=*moved;auto& recipe=corrupt.metadata["earth/infrastructure/part/1"];
    recipe.resize(recipe.rfind(' '));CHECK_FALSE(earth::infrastructure_parts(corrupt));
}
TEST_CASE("Earth is a reproducible ordinary mesh with smooth normals and layered geometry", "[earth][assets]") {
    const auto mesh=earth::make_mesh();REQUIRE(content::vmesh::validate(mesh));
    CHECK(mesh==earth::make_mesh());
    // Dispersed fringes introduce more clipped outline vertices than joined
    // banks. Keep the default comfortably inside the unchanged 65,536 budget.
    // Independent formations retain separate outlines where banks overlap.
    CHECK(mesh.vertex_count<46000);CHECK(mesh.faces.size()<75000);
    const auto& p=values(mesh,"position"),&n=values(mesh,"normal");
    const auto& color=values(mesh,"color/0"),&emission=values(mesh,"emission");
    std::size_t ocean{},land{},cloud{},blue{},green{},white{},invalid{},billows{};
    f32 lowest_cloud=2,highest_cloud=0,lowest_cloud_alignment=1;
    for(std::size_t i=0;i<mesh.vertex_count;++i) {
        const auto v=at(p,i),normal=at(n,i);
        const auto radius=std::sqrt(dot(v,v));
        // Clouds now sit closer to terrain; classify by this asset's layer
        // emission convention instead of assuming the old altitude separation.
        const auto is_cloud=emission[i]<.02F;
        invalid+=!std::isfinite(radius)||!std::isfinite(dot(normal,normal))||radius<.999F||radius>1.04F||
            std::abs(dot(normal,normal)-1)>1e-4F||dot(normal,unit(v))<(is_cloud?.8F:.999F);
        if(is_cloud) {
            lowest_cloud=std::min(lowest_cloud,radius);highest_cloud=std::max(highest_cloud,radius);
            lowest_cloud_alignment=std::min(lowest_cloud_alignment,dot(normal,unit(v)));
            billows+=dot(normal,unit(v))<.999F;
        }
        ocean+=!is_cloud&&radius<1.002F;land+=!is_cloud&&radius>=1.002F;cloud+=is_cloud;
        const auto r=color[i*4],g=color[i*4+1],b=color[i*4+2];
        blue+=b>r*2&&b>g*1.5F;green+=g>r*1.5F&&g>b*1.4F;white+=r>.5F&&g>.5F&&b>.5F;
    }
    CAPTURE(lowest_cloud,highest_cloud,lowest_cloud_alignment);
    CHECK(invalid==0);CHECK(ocean>1000);CHECK(land>1000);CHECK(cloud>500);
    CHECK(blue>1000);CHECK(green>500);CHECK(white>500);
    CHECK(billows>500);
    CHECK(lowest_cloud>1.015F);CHECK(highest_cloud<1.021F);
    CHECK(highest_cloud-lowest_cloud>.0015F);CHECK(highest_cloud-lowest_cloud<.005F);
    f32 longest_land_edge{},longest_cloud_edge{};
    for(const auto face:mesh.faces) {
        const auto a=at(p,face[0]),b=at(p,face[1]),c=at(p,face[2]);
        CHECK(dot(cross(sub(b,a),sub(c,a)),a)>0.F);
        const auto radius=std::sqrt(dot(a,a));
        for(unsigned edge=0;edge<3;++edge) {
            // Cloud normals now follow puff slopes rather than the sphere.
            // Measure tessellation by radial positions, not shading normals.
            const auto delta=sub(unit(at(p,face[edge])),unit(at(p,face[(edge+1)%3])));
            const auto length=std::sqrt(dot(delta,delta));
            if(emission[face[0]]>=.02F&&radius>=1.003F)longest_land_edge=std::max(longest_land_edge,length);
            if(emission[face[0]]<.02F)longest_cloud_edge=std::max(longest_cloud_edge,length);
        }
    }
    // Guard actual contour precision, not just an inflated vertex count. Ocean
    // tessellation may remain economical; land and weather need finer samples.
    CHECK(longest_land_edge>0.F);CHECK(longest_land_edge<.036F);
    CHECK(longest_cloud_edge>0.F);CHECK(longest_cloud_edge<.021F);
    auto serialized=content::vmesh::write_vmesh(mesh);REQUIRE(serialized);
    auto restored=content::vmesh::parse_vmesh(*serialized);REQUIRE(restored);CHECK(*restored==mesh);
}
TEST_CASE("Savannah Earth is a subtle land-only copy with unchanged geometry and clouds", "[earth][savannah]") {
    auto original=content::vmesh::read_vmesh(std::filesystem::path(VNG_EARTH_ASSETS)/"earth.vmesh");REQUIRE(original);
    if(GENERATE(false,true))*original=earth::make_mesh(); // Legacy emission stamps and modern ownership.
    auto copy=earth::make_savannah_variant(*original);REQUIRE(copy);
    REQUIRE(content::vmesh::validate(*copy));CHECK(earth::is_earth(*copy));
    CHECK(copy->vertex_count==original->vertex_count);CHECK(copy->faces==original->faces);CHECK(copy->edges==original->edges);
    for(const auto& [key,value]:original->metadata)if(key!="name"&&key!="style")CHECK(copy->metadata.at(key)==value);
    for(const auto& field:original->vertex_fields)if(field.name!="color/0") {
        const auto found=std::ranges::find(copy->vertex_fields,field.name,&content::vmesh::VertexField::name);
        REQUIRE(found!=copy->vertex_fields.end());CHECK(*found==field);
    }
    const auto& positions=values(*original,"position"),&emission=values(*original,"emission");
    const auto& before=values(*original,"color/0"),&after=values(*copy,"color/0");
    std::size_t changed{},green{},warmer{},nonland_changed{},alpha_changed{};float max_delta{};
    for(std::size_t i=0;i<original->vertex_count;++i) {
        const auto p=at(positions,i);const bool land=emission[i]>.03F&&dot(p,p)>1.002F*1.002F;
        const bool differs=before[i*4]!=after[i*4]||before[i*4+1]!=after[i*4+1]||before[i*4+2]!=after[i*4+2];
        if(!land)nonland_changed+=differs;else changed+=differs;
        if(land&&before[i*4+1]>before[i*4]+.07F&&before[i*4+1]>before[i*4+2]+.04F) {
            ++green;warmer+=after[i*4]>before[i*4]+.01F;
        }
        for(unsigned c=0;c<3;++c)max_delta=std::max(max_delta,std::abs(before[i*4+c]-after[i*4+c]));
        alpha_changed+=before[i*4+3]!=after[i*4+3];
    }
    CHECK(changed>1000);CHECK(green>500);CHECK(warmer>green*.9);
    CHECK(nonland_changed==0);CHECK(alpha_changed==0);CHECK(max_delta<.15F);
    CHECK_FALSE(original->metadata.contains("editor/earth-variant"));
}
TEST_CASE("Cloud editing preserves the whole mesh authoring frame and untouched terrain", "[earth][mesh-frame]") {
    const auto source=earth::make_mesh();
    auto frame=Mat4::identity();
    frame[0]={0,0,-1.6F,0};frame[1]={0,.8F,0,0};frame[2]={2,0,0,0};frame[3]={3,-2,4,1};
    auto inverse=example::mesh_frame::inverse(frame);REQUIRE(inverse);
    const Vec3 test{.3F,-.7F,1.5F};
    const auto restored=example::mesh_frame::point(*inverse,example::mesh_frame::point(frame,test));
    CHECK(std::hypot(restored.x-test.x,restored.y-test.y,restored.z-test.z)<1e-5F);
    auto transformed=example::mesh_frame::transformed(source,frame);REQUIRE(transformed);
    auto saved=content::vmesh::write_vmesh(*transformed);REQUIRE(saved);
    auto loaded=content::vmesh::parse_vmesh(*saved);REQUIRE(loaded);
    REQUIRE(example::mesh_frame::read(*loaded));CHECK(*example::mesh_frame::read(*loaded)==frame);
    const auto ownership=std::ranges::find(source.vertex_fields,"earth/cloud",&content::vmesh::VertexField::name);
    REQUIRE(ownership!=source.vertex_fields.end());const auto& owners=std::get<std::vector<u32>>(ownership->values);
    auto moved=earth::move_cloud(*loaded,15,{15,20});REQUIRE(moved);
    auto canonical_move=earth::move_cloud(source,15,{15,20});REQUIRE(canonical_move);
    auto expected=example::mesh_frame::transformed(*canonical_move,frame);REQUIRE(expected);
    for(auto field:{"position","normal"}) {
        const auto& before=values(*loaded,field),&after=values(*moved,field),&wanted=values(*expected,field);
        for(std::size_t i=0;i<source.vertex_count;++i) {
            if(owners[i]!=15)CHECK(at(after,i)==at(before,i));
            else {const auto delta=sub(at(after,i),at(wanted,i));CHECK(std::hypot(delta.x,delta.y,delta.z)<1e-5F);}
        }
    }
    for(bool height_only:{false,true}) {
        auto settings=*earth::cloud_settings(source);
        if(height_only)settings.altitude=2;else settings.puff_size=1.2F;
        auto rebuilt=earth::rebuild_clouds(*moved,settings);REQUIRE(rebuilt);
        auto plain=earth::rebuild_clouds(*canonical_move,settings);REQUIRE(plain);
        auto wanted=example::mesh_frame::transformed(*plain,frame);REQUIRE(wanted);
        CHECK(rebuilt->vertex_count==wanted->vertex_count);
        for(auto field:{"position","normal"}) {
            const auto& after=values(*rebuilt,field),&expected_values=values(*wanted,field);
            for(std::size_t i=0;i<rebuilt->vertex_count;++i) {
                const auto delta=sub(at(after,i),at(expected_values,i));
                CHECK(std::hypot(delta.x,delta.y,delta.z)<(height_only?.002F:1e-5F));
            }
        }
        // Retained terrain was never inverse/forward transformed by the recipe.
        const auto& p=values(*rebuilt,"position");std::size_t next{};
        for(std::size_t i=0;i<owners.size();++i)if(!owners[i]) {
            CHECK(at(p,next)==at(values(*moved,"position"),i));++next;
        }
    }
    auto invalid=frame;invalid[0]={0,0,0,0};CHECK_FALSE(example::mesh_frame::inverse(invalid));
}
TEST_CASE("Earth land includes the major continents without filling the oceans", "[earth][geography]") {
    const auto mesh=earth::make_mesh();const auto& p=values(mesh,"position"),&n=values(mesh,"normal");
    const auto& emission=values(mesh,"emission");
    auto nearby_land=[&](Vec2 location) {
        const auto lon=location.x*std::numbers::pi_v<f32>/180,lat=location.y*std::numbers::pi_v<f32>/180;
        const Vec3 direction{std::sin(lon)*std::cos(lat),std::sin(lat),std::cos(lon)*std::cos(lat)};
        f32 closest=-1;
        for(std::size_t i=0;i<mesh.vertex_count;++i) {
            const auto point=at(p,i);
            const auto r=std::sqrt(dot(point,point));
            if(r>1.003F&&emission[i]>=.02F)closest=std::max(closest,dot(at(n,i),direction));
        }
        return closest;
    };
    for(auto point:{Vec2{-100,45},Vec2{-60,-10},Vec2{20,0},Vec2{90,45},Vec2{135,-25}})CHECK(nearby_land(point)>.997F);
    // These sample points are at least five degrees offshore. A denser mesh
    // finds the Brazilian coast more accurately than the old coarse grid did.
    for(auto point:{Vec2{-30,0},Vec2{-130,0},Vec2{75,-25}}) {
        CAPTURE(point.x,point.y);
        CHECK(nearby_land(point)<std::cos(5.F*std::numbers::pi_v<f32>/180));
    }
    // Refinement must retain recognizable small islands and narrow peninsulas.
    for(auto point:{Vec2{9,40},Vec2{14,37.5F},Vec2{121,24},Vec2{-56,49},Vec2{-81,27}}) {
        CAPTURE(point.x,point.y);CHECK(nearby_land(point)>.9998F);
    }
}
TEST_CASE("Earth blueprint has normal instance ownership animation and import semantics", "[earth][editor]") {
    auto scene=earth::author_scene(VNG_EARTH_ASSETS);REQUIRE(scene);
    const auto* mesh=project::mesh_geometry(*scene,earth::blueprint_id);REQUIRE(mesh);
    CHECK(project::evaluate_transform(*scene,*project::find_instance(*scene,1),45).rotation.y==120.F);
    auto bytes=project::encode_scene(*scene);REQUIRE(bytes);auto restored=project::decode(*bytes);REQUIRE(restored);
    CHECK(project::mesh_geometry(*restored,earth::blueprint_id)->document()==mesh->document());
    project::EditingSession editing{*scene};
    auto second=editing.instantiate(earth::blueprint_id);REQUIRE(second);
    CHECK(project::find_instance(editing.state(),*second)->blueprint==earth::blueprint_id);
    REQUIRE(editing.undo());CHECK(editing.state().document.instances.size()==2); // Earth and its camera
    REQUIRE(editing.redo());CHECK(editing.state().document.instances.size()==3);
    REQUIRE(project::begin_mesh_draft(*scene,earth::blueprint_id));
    CHECK(project::mesh_edit_geometry(*scene,earth::blueprint_id)!=project::mesh_geometry(*scene,earth::blueprint_id));
    // A close-up mesh must still fit both applied geometry and an editable draft
    // in the scene/worker payload. More triangles alone is not a usable upgrade.
    auto draft_bytes=project::encode(*scene);REQUIRE(draft_bytes);
    auto draft_restored=project::decode(*draft_bytes);
    INFO((draft_restored?"draft decoded":draft_restored.error().message));REQUIRE(draft_restored);
    CHECK(project::mesh_edit_geometry(*draft_restored,earth::blueprint_id)->document()==mesh->document());
    // The shipped standalone mesh goes through the same user import path.
    auto imported=project::import_mesh(*restored,std::filesystem::path(VNG_EARTH_ASSETS)/"earth.vmesh");REQUIRE(imported);
    CHECK(project::instance_mesh(*restored,*imported)->document().metadata.at("render/lighting")=="illustrated");
    // The authoring recipe may improve, but importing old baked clouds must not
    // silently regenerate them. Compare with the saved asset, not today's recipe.
    const auto legacy=editor::EditableMesh::load(std::filesystem::path(VNG_EARTH_ASSETS)/"earth.vmesh");REQUIRE(legacy);
    const auto& imported_mesh=project::instance_mesh(*restored,*imported)->document();
    CHECK(imported_mesh==legacy->document());
    REQUIRE(earth::cloud_settings(imported_mesh));CHECK(*earth::cloud_settings(imported_mesh)==earth::CloudSettings{.edge_scatter=0});
}
TEST_CASE("Earth dry regions blend into vegetation with finer painted surface variation", "[earth][biomes]") {
    const auto mesh=earth::make_mesh();
    const auto& p=values(mesh,"position"),&n=values(mesh,"normal"),&colors=values(mesh,"color/0");
    const auto& emission=values(mesh,"emission");
    auto land_color=[&](Vec2 location) {
        const auto lon=location.x*std::numbers::pi_v<f32>/180,lat=location.y*std::numbers::pi_v<f32>/180;
        const Vec3 direction{std::sin(lon)*std::cos(lat),std::sin(lat),std::cos(lon)*std::cos(lat)};
        f32 closest=-1;Vec3 result{};
        for(std::size_t i=0;i<mesh.vertex_count;++i) {
            const auto point=at(p,i);
            const auto radius=std::sqrt(dot(point,point));
            const auto proximity=dot(at(n,i),direction);
            if(radius>1.003F&&emission[i]>=.02F&&proximity>closest) {
                closest=proximity;result={colors[i*4],colors[i*4+1],colors[i*4+2]};
            }
        }
        REQUIRE(closest>.999F);return result;
    };
    // Sahara, Arabia, Taklamakan, Gobi, northern Mexico and southern Africa.
    for(const auto point:{Vec2{15,24},Vec2{48,23},Vec2{83,40},Vec2{104,43},Vec2{-106,28},Vec2{20,-24}}) {
        CAPTURE(point.x,point.y);const auto color=land_color(point);
        CHECK(color.x>color.y*1.15F);CHECK(color.y>color.z*1.5F);
    }
    // Dryness must not become a blanket tint over the whole planet.
    for(const auto point:{Vec2{-60,0},Vec2{22,0},Vec2{-90,40},Vec2{-90,16}}) {
        CAPTURE(point.x,point.y);const auto color=land_color(point);
        CHECK(color.y>color.x*1.4F);
    }
    f32 minimum_red=1,maximum_red=0;
    for(int lon=4;lon<=16;lon+=2)for(int lat=20;lat<=28;lat+=2) {
        const auto color=land_color({static_cast<f32>(lon),static_cast<f32>(lat)});
        minimum_red=std::min(minimum_red,color.x);maximum_red=std::max(maximum_red,color.x);
    }
    CHECK(maximum_red-minimum_red>.04F); // Variation within sand, not just at biome edges.
    auto previous=land_color({15,8});unsigned intermediate{};
    for(int latitude=9;latitude<=24;++latitude) {
        CAPTURE(latitude);
        const auto color=land_color({15,static_cast<f32>(latitude)});
        CHECK(std::abs(color.x-previous.x)<.13F);
        intermediate+=color.x>.17F&&color.x<.4F;
        previous=color;
    }
    CHECK(intermediate>=2); // Visible scrub transition instead of a green/sand step.
}
TEST_CASE("Earth cloud authoring controls affect independent particle properties", "[earth][clouds]") {
    earth::CloudParticles original,thicker{{.coverage=1.6F}},larger{{.puff_size=1.4F}},spiral{{.spiral_size=1.5F}},
        higher{{.altitude=2}},flat{{.relief=0}},hidden{{.visible=false}};
    const auto& p=original.particles().front();
    CHECK(larger.particles().front().radius>p.radius);
    CHECK(thicker.sample(p.center).density>original.sample(p.center).density);
    CHECK(higher.sample(p.center).radius>original.sample(p.center).radius);
    CHECK(flat.sample(p.center).normal==p.center);
    CHECK(hidden.particles().empty());
    CHECK(original.particles().front()==spiral.particles().front());
    CHECK(original.particles().back().center!=spiral.particles().back().center);
    auto generated=earth::make_mesh();
    auto rebuilt=earth::rebuild_clouds(generated,{.coverage=1.6F,.puff_size=1.4F,.spiral_size=1.5F,.altitude=2,.relief=.4F});
    REQUIRE(rebuilt);REQUIRE(content::vmesh::validate(*rebuilt));
    CHECK(rebuilt->faces!=generated.faces);
    CHECK(earth::cloud_settings(*rebuilt)->puff_size==1.4F);
    CHECK(rebuilt->vertex_count<65536);
    auto published=editor::EditableMesh::create(generated);REQUIRE(published);
    auto draft=editor::EditableMesh::create(*rebuilt);REQUIRE(draft);
    project::State state{.document={.mesh=std::move(*published)}};
    state.document.mesh_drafts.emplace(project::BlueprintId::mesh,std::move(*draft));
    auto bytes=project::encode_scene(state);REQUIRE(bytes);
    CHECK(bytes->size()>16U*1024U*1024U); // The real published+draft regression, not synthetic padding.
    auto restored=project::decode(*bytes);REQUIRE(restored);
    CHECK(project::mesh_edit_geometry(*restored,project::BlueprintId::mesh)->document()==*rebuilt);
}
TEST_CASE("Cloud scatter grows toward the fringe without rerolling the dense core", "[earth][clouds]") {
    const earth::CloudParticles original{{.edge_scatter=0}},gentle{{.edge_scatter=.5F}},scattered{{.edge_scatter=1}},wide{{.edge_scatter=2}};
    const auto base=original.particles(),low=gentle.particles(),mid=scattered.particles(),high=wide.particles();
    REQUIRE(base.size()==low.size());REQUIRE(base.size()==mid.size());REQUIRE(base.size()==high.size());
    std::size_t moved{},still{};
    for(std::size_t i=0;i<base.size();++i) {
        const auto distance=[&](auto p){const auto d=sub(p.center,base[i].center);return std::sqrt(dot(d,d));};
        const auto a=distance(low[i]),b=distance(mid[i]),c=distance(high[i]);
        CHECK(a<=b+1e-6F);CHECK(b<=c+1e-6F);
        CHECK(mid[i].strength==base[i].strength);CHECK(mid[i].height==base[i].height);
        CHECK(mid[i].radius<=base[i].radius);
        CHECK(std::abs(dot(high[i].center,high[i].center)-1)<1e-5F);
        CHECK(std::isfinite(wide.sample(high[i].center).density));
        moved+=b>.001F;still+=b<1e-6F;
    }
    CHECK(moved>base.size()/2);CHECK(still>0);
    // The first bank's curved spine is authored at (-42,45), angle 18 degrees,
    // half-length 23 and width 3. Compare central puffs to its flanks/endcaps.
    constexpr f32 rad=std::numbers::pi_v<f32>/180;
    f32 core_distance{},edge_distance{},previous=-100;
    unsigned core_count{},edge_count{};
    for(std::size_t i=0;i<base.size();++i) {
        const auto p=base[i].center;
        const auto lon=std::atan2(p.x,p.z)/rad,lat=std::asin(p.y)/rad;
        const auto dx=(lon+42)*std::cos(45*rad),dy=lat-45;
        const auto along=dx*std::cos(18*rad)+dy*std::sin(18*rad);
        if(along<previous-3)break; // Next bank starts a fresh longitudinal sweep.
        previous=along;
        const auto across=-dx*std::sin(18*rad)+dy*std::cos(18*rad)-2.25F*std::sin(along/23*3);
        const auto end=std::abs(along/23);
        const auto side=std::abs(across)/(1.95F*std::max(.2F,std::sqrt(std::max(0.F,1-end*end))));
        const auto d=sub(mid[i].center,p);
        const auto distance=std::sqrt(dot(d,d));
        if(end<.4F&&side<.4F){core_distance+=distance;++core_count;}
        if(end>.8F||side>.8F){edge_distance+=distance;++edge_count;}
    }
    REQUIRE(core_count>1);REQUIRE(edge_count>10);
    CHECK(edge_distance/static_cast<f32>(edge_count)>4*core_distance/static_cast<f32>(core_count));
}
TEST_CASE("Spiral clouds have open eyes broken inner clusters and outer bands", "[earth][clouds]") {
    const auto scatter=GENERATE(0.F,1.F,2.F);
    const earth::CloudParticles clouds{{.edge_scatter=scatter}};
    constexpr f32 radians=std::numbers::pi_v<f32>/180;
    const auto direction=[&](Vec2 p) {
        return Vec3{std::sin(p.x*radians)*std::cos(p.y*radians),std::sin(p.y*radians),std::cos(p.x*radians)*std::cos(p.y*radians)};
    };
    for(const auto center:{Vec2{-46,26},Vec2{79,-34},Vec2{-161,-12}}) {
        CAPTURE(scatter,center.x,center.y);
        CHECK(clouds.sample(direction(center)).density<earth::CloudParticles::boundary);
        unsigned inner{},outer{};
        for(int i=0;i<48;++i) {
            const auto angle=2*std::numbers::pi_v<f32>*static_cast<f32>(i)/48;
            const auto sample=[&](f32 radius) {
                return clouds.sample(direction({center.x+std::cos(angle)*radius/std::cos(center.y*radians),
                    center.y+std::sin(angle)*radius})).density;
            };
            inner+=sample(3.F*1.25F)>earth::CloudParticles::boundary;
            outer+=sample(8.F*1.25F)>earth::CloudParticles::boundary;
        }
        CHECK(inner>8);CHECK(inner<42); // Billows with actual gaps, never an opaque disk.
        CHECK(outer>2);CHECK(outer<36); // Readable cloud bands with open ocean between them.
    }
}
TEST_CASE("Cloud heading and the editable formation catalog survive rebuilding and disk roundtrips", "[earth][cloud-catalog]") {
    const auto original=earth::make_mesh();
    auto turned=earth::rotate_cloud(original,15,35);REQUIRE(turned);
    REQUIRE(content::vmesh::validate(*turned));
    const auto before=earth::cloud_formations(original);REQUIRE(before);
    const auto after=earth::cloud_formations(*turned);REQUIRE(after);
    CHECK(std::abs(after->at(14).location.x-before->at(14).location.x)<.001F);
    CHECK(std::abs(after->at(14).location.y-before->at(14).location.y)<.001F);
    CHECK(turned->faces==original.faces);CHECK(turned->vertex_count==original.vertex_count);
    const auto& owners=std::get<std::vector<u32>>(std::ranges::find(original.vertex_fields,"earth/cloud",&content::vmesh::VertexField::name)->values);
    bool changed{};
    for(std::size_t i=0;i<original.vertex_count;++i) {
        if(owners[i]!=15) {
            REQUIRE(at(values(*turned,"position"),i)==at(values(original,"position"),i));
            REQUIRE(at(values(*turned,"normal"),i)==at(values(original,"normal"),i));
        } else changed|=at(values(*turned,"position"),i)!=at(values(original,"position"),i);
    }
    CHECK(changed);
    auto rebuilt=earth::rebuild_clouds(*turned,*earth::cloud_settings(*turned));REQUIRE(rebuilt);
    CHECK(values(*rebuilt,"position")==values(*turned,"position"));
    auto added=earth::add_cloud(*turned,earth::CloudKind::spiral,{20,30});REQUIRE(added);
    REQUIRE(content::vmesh::validate(*added));
    auto formations=earth::cloud_formations(*added);REQUIRE(formations);REQUIRE(formations->size()==18);
    const auto id=formations->back().id;CHECK(id>17);
    CHECK(std::abs(formations->back().location.x-20)<.001F);CHECK(std::abs(formations->back().location.y-30)<.001F);
    auto remove=earth::remove_cloud(*added,15);REQUIRE(remove);
    formations=earth::cloud_formations(*remove);REQUIRE(formations);
    CHECK(std::ranges::find(*formations,15U,&earth::CloudFormation::id)==formations->end());
    CHECK_FALSE(earth::move_cloud(*remove,15,{0,0}));CHECK_FALSE(earth::rotate_cloud(*remove,15,5));
    auto settings=*earth::cloud_settings(*remove);settings.visible=false;
    auto hidden=earth::rebuild_clouds(*remove,settings);REQUIRE(hidden);
    settings.visible=true;settings.puff_size=1.1F;
    rebuilt=earth::rebuild_clouds(*hidden,settings);REQUIRE(rebuilt);
    formations=earth::cloud_formations(*rebuilt);REQUIRE(formations);CHECK(formations->size()==17);
    CHECK(std::ranges::find(*formations,15U,&earth::CloudFormation::id)==formations->end());
    auto text=content::vmesh::write_vmesh(*rebuilt);REQUIRE(text);auto loaded=content::vmesh::parse_vmesh(*text);REQUIRE(loaded);
    auto moved=earth::move_cloud(*loaded,id,{-20,10});REQUIRE(moved);
    CHECK(earth::rotate_cloud(*moved,id,-30));
    remove=earth::remove_cloud(*moved,id);REQUIRE(remove);
    added=earth::add_cloud(*remove,earth::CloudKind::bank);REQUIRE(added);
    formations=earth::cloud_formations(*added);REQUIRE(formations);CHECK(formations->back().id>id);
    // Invalid recipes are rejected before indexing or allocating from IDs.
    auto bad=*added;bad.metadata["earth/clouds/template/999999999"]="1";
    CHECK_FALSE(earth::cloud_formations(bad));CHECK_FALSE(earth::rebuild_clouds(bad,settings));
}
TEST_CASE("Whole cloud formations move rigidly without editing their neighbors", "[earth][cloud-placement]") {
    auto original=earth::make_mesh();
    REQUIRE(earth::has_cloud_ownership(original));
    const auto owner=std::ranges::find(original.vertex_fields,"earth/cloud",&content::vmesh::VertexField::name);
    REQUIRE(owner!=original.vertex_fields.end());
    const auto ids=std::get<std::vector<u32>>(owner->values);
    std::array<std::size_t,18> counts{};
    for(const auto id:ids){REQUIRE(id<counts.size());++counts[id];}
    for(auto count:counts)CHECK(count>0);
    for(const auto face:original.faces) {
        REQUIRE(ids[face[0]]==ids[face[1]]);REQUIRE(ids[face[0]]==ids[face[2]]);
    }
    // A hand edit and an unrelated custom attribute must survive relocation.
    const auto first=static_cast<std::size_t>(std::ranges::find(ids,15U)-ids.begin());
    std::get<std::vector<f32>>(original.vertex_fields[0].values)[first*3]*=1.0002F;
    original.vertex_fields.push_back({"custom/author",{content::vmesh::ScalarType::UInt32,1},std::vector<u32>(original.vertex_count,42)});
    auto moved=earth::move_cloud(original,15,{15,12});REQUIRE(moved);
    CHECK(moved->faces==original.faces);CHECK(moved->edges==original.edges);
    CHECK(moved->vertex_count==original.vertex_count);
    bool changed{};
    for(std::size_t f=0;f<original.vertex_fields.size();++f) {
        const auto& field=original.vertex_fields[f];
        if(field.name!="position" && field.name!="normal") {CHECK(field==moved->vertex_fields[f]);continue;}
        const auto& before=std::get<std::vector<f32>>(field.values);
        const auto& after=std::get<std::vector<f32>>(moved->vertex_fields[f].values);
        for(std::size_t i=0;i<ids.size();++i) {
            if(ids[i]!=15) {REQUIRE(at(before,i)==at(after,i));continue;}
            changed|=at(before,i)!=at(after,i);
            REQUIRE(std::abs(dot(at(before,i),at(before,i))-dot(at(after,i),at(after,i)))<1e-5F);
        }
    }
    CHECK(changed);
    const auto& before=values(original,"position");const auto& after=values(*moved,"position");
    for(auto face:original.faces)if(ids[face[0]]==15) {
        const auto a=sub(at(before,face[0]),at(before,face[1])),b=sub(at(after,face[0]),at(after,face[1]));
        REQUIRE(std::abs(dot(a,a)-dot(b,b))<1e-6F);
    }
    auto formation=earth::cloud_formations(*moved);REQUIRE(formation);
    CHECK(std::abs((*formation)[14].location.x-15)<1e-4F);CHECK(std::abs((*formation)[14].location.y-12)<1e-4F);
    CHECK(earth::move_cloud(*moved,15,(*formation)[14].location)->vertex_fields==moved->vertex_fields);
    // Both poles, dateline crossings and a truly antipodal move stay finite.
    for(auto location:{Vec2{179,0},Vec2{-179,0},Vec2{0,90},Vec2{0,-90},Vec2{134,-26},Vec2{133.95F,-26}}) {
        CAPTURE(location.x,location.y);
        auto next=earth::move_cloud(original,15,location);REQUIRE(next);REQUIRE(content::vmesh::validate(*next));
        auto described=earth::cloud_formations(*next);REQUIRE(described);
        CHECK(std::abs((*described)[14].location.y-location.y)<.03F);
        if(std::abs(location.y)<89)CHECK(std::abs((*described)[14].location.x-location.x)<.001F);
    }
}
TEST_CASE("Cloud placement persists through rebuild hiding draft history and serialization", "[earth][cloud-placement]") {
    const auto original=earth::make_cloud_mesh({});
    auto moved=earth::move_cloud(original,15,{25,15});REQUIRE(moved);
    moved=earth::move_cloud(*moved,15,{-20,60});REQUIRE(moved);
    auto rebuilt=earth::rebuild_clouds(*moved,{});REQUIRE(rebuilt);
    CHECK(rebuilt->faces==moved->faces);
    for(const auto name:{"position","normal"}) {
        const auto& a=values(*moved,name);const auto& b=values(*rebuilt,name);REQUIRE(a.size()==b.size());
        for(std::size_t i=0;i<a.size();++i)REQUIRE(std::abs(a[i]-b[i])<1e-5F);
    }
    auto hidden=earth::rebuild_clouds(*moved,{.visible=false});REQUIRE(hidden);CHECK(hidden->vertex_count==0);
    hidden=earth::move_cloud(*hidden,15,{80,-20});REQUIRE(hidden);
    auto restored=earth::rebuild_clouds(*hidden,{});REQUIRE(restored);
    CHECK(std::abs(earth::cloud_formations(*restored)->at(14).location.x-80)<.001F);
    auto start=editor::EditableMesh::create(original);REQUIRE(start);
    project::EditingSession session{project::State{.document={.mesh=std::move(*start)}}};
    auto draft=editor::EditableMesh::create(*moved);REQUIRE(draft);
    REQUIRE(session.replace_mesh_draft(project::BlueprintId::mesh,session.state().document.revision,std::move(*draft)));
    CHECK(session.state().document.mesh.document()==original);
    auto encoded=project::encode_scene(session.state());REQUIRE(encoded);
    auto decoded=project::decode(*encoded);REQUIRE(decoded);
    CHECK(project::mesh_edit_geometry(*decoded,project::BlueprintId::mesh)->document()==*moved);
    REQUIRE(session.undo());CHECK(session.state().document.mesh_drafts.empty());
    REQUIRE(session.redo());REQUIRE(session.apply_mesh(project::BlueprintId::mesh));
    CHECK(session.state().document.mesh.document()==*moved);
}
TEST_CASE("Cloud movement rejects ambiguous ownership invalid positions and joined formations", "[earth][cloud-placement]") {
    auto mesh=earth::make_cloud_mesh({});
    CHECK_FALSE(earth::move_cloud(mesh,0,{0,0}));CHECK_FALSE(earth::move_cloud(mesh,18,{0,0}));
    CHECK_FALSE(earth::move_cloud(mesh,15,{181,0}));CHECK_FALSE(earth::move_cloud(mesh,15,{0,91}));
    CHECK_FALSE(earth::move_cloud(mesh,15,{std::numeric_limits<f32>::quiet_NaN(),0}));
    auto bad=mesh;bad.metadata["earth/clouds/formation/15/rotation"]="0 0 0 0";
    CHECK_FALSE(earth::move_cloud(bad,15,{0,0}));CHECK_FALSE(earth::rebuild_clouds(bad,{}));
    const auto& ids=std::get<std::vector<u32>>(std::ranges::find(mesh.vertex_fields,"earth/cloud",&content::vmesh::VertexField::name)->values);
    const auto selected=static_cast<u32>(std::ranges::find(ids,15U)-ids.begin());
    bad=mesh;bad.faces.emplace_back(selected,0,1);CHECK_FALSE(earth::move_cloud(bad,15,{0,0}));
    bad=mesh;bad.edges=std::vector<gfx::Edge>{{selected,0}};CHECK_FALSE(earth::move_cloud(bad,15,{0,0}));
    std::erase_if(mesh.vertex_fields,[](const auto& f){return f.name=="earth/cloud";});
    CHECK_FALSE(earth::has_cloud_ownership(mesh));CHECK_FALSE(earth::move_cloud(mesh,15,{0,0}));
    auto migrated=earth::rebuild_clouds(mesh,{});REQUIRE(migrated);CHECK(earth::move_cloud(*migrated,15,{0,0}));
}
TEST_CASE("Cloud edits retain narrow change identity through history and preview transport", "[earth][cloud-update]") {
    auto original=earth::make_mesh();auto source=editor::EditableMesh::create(original);REQUIRE(source);
    project::State initial{.document={.mesh=std::move(*source)}};initial.viewport.mode=project::ViewMode::mesh;
    project::EditingSession session{initial};project::PreviewDeliveryLogic updates;updates.add(1);updates.accepted(1,initial.document.revision);
    const auto start=std::chrono::steady_clock::now();
    auto moved=earth::move_cloud(original,15,{-15,5});REQUIRE(moved);
    auto next=editor::EditableMesh::create(*moved);REQUIRE(next);
    REQUIRE(session.replace_mesh_draft(project::BlueprintId::mesh,session.state().document.revision,std::move(*next)));
    auto notice=session.take_changes();REQUIRE(notice);CHECK_FALSE(notice->changes.full);
    REQUIRE(notice->changes.meshes.size()==1);const auto& scope=notice->changes.meshes.begin()->second;
    CHECK_FALSE(scope.whole);CHECK(scope.fields.size()==2);CHECK(scope.metadata.size()==1);
    updates.changed(notice->changes);auto packet=updates.next(1,session.state());REQUIRE(packet);REQUIRE(*packet);
    CHECK((**packet).starts_with("patch\n"));CHECK((**packet).size()<150000);
    auto patch=project::decode_patch(std::string_view(**packet).substr(6));REQUIRE(patch);REQUIRE(project::apply_patch(initial,*patch));
    CHECK(project::editable_mesh(initial)->document()==*moved);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"Cloud move + history + binary transport + worker apply: "<<ms<<" ms / "<<(**packet).size()<<" bytes\n";
    auto settings=*earth::cloud_settings(*moved);settings.altitude=1.5F;settings.relief=.5F;
    const auto height_start=std::chrono::steady_clock::now();
    auto raised=earth::rebuild_clouds(*moved,settings);REQUIRE(raised);
    const auto delta=editor::mesh_changes(*moved,*raised);
    CHECK_FALSE(delta.whole);CHECK(delta.fields.size()==2);CHECK(raised->faces==moved->faces);
    CHECK(raised->vertex_count==moved->vertex_count);
    CHECK(values(*raised,"color/0")==values(*moved,"color/0"));
    const auto& owners=std::get<std::vector<u32>>(std::ranges::find(moved->vertex_fields,"earth/cloud",&content::vmesh::VertexField::name)->values);
    for(const auto& [name,ids]:delta.fields)for(auto id:ids)REQUIRE(owners[id]!=0);
    std::cout<<"Cloud height edit preserving topology: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-height_start).count()<<" ms\n";
}
TEST_CASE("Earth cloud particles merge reproducibly and spatial bins preserve every contribution", "[earth][clouds]") {
    const auto scatter=GENERATE(0.F,1.F,2.F);
    const earth::CloudParticles clouds{{.edge_scatter=scatter}},second{{.edge_scatter=scatter}};
    REQUIRE(std::ranges::equal(clouds.particles(),second.particles()));
    CHECK(clouds.particles().size()>500);CHECK(clouds.particles().size()<2000);
    auto check_sample=[&](Vec3 n) {
        const auto actual=clouds.sample(n);
        f32 density{},weighted_height{};
        for(const auto& p:clouds.particles()) {
            const auto delta=sub(n,p.center);
            const auto q=dot(delta,delta)/(p.radius*p.radius);
            if(q>=1)continue;
            const auto t=1-q,weight=p.strength*t*t*t;
            density+=weight;weighted_height+=weight*p.height;
        }
        CHECK(std::abs(actual.density-density)<1e-5F);
        const auto flattened_altitude=.019F+.006F+.5F*(weighted_height/(.7F+density)-.006F);
        CHECK(std::abs((actual.radius-1.F)-.7F*flattened_altitude)<1e-6F);
        CHECK(actual.radius>1.015F);CHECK(actual.radius<1.021F);
        CHECK(std::abs(dot(actual.normal,actual.normal)-1.F)<1e-5F);
        CHECK(dot(actual.normal,n)>.8F);
    };
    for(int latitude=-90;latitude<=90;latitude+=5)for(int longitude=-180;longitude<=180;longitude+=5) {
        const auto lat=static_cast<f32>(latitude)*std::numbers::pi_v<f32>/180;
        const auto lon=static_cast<f32>(longitude)*std::numbers::pi_v<f32>/180;
        check_sample({std::sin(lon)*std::cos(lat),std::sin(lat),std::cos(lon)*std::cos(lat)});
    }
    for(std::size_t i=0;i<clouds.particles().size();i+=7) {
        const auto& particle=clouds.particles()[i];
        check_sample(particle.center);
        CHECK(clouds.sample(particle.center).density>=particle.strength);
        // Compare the softened analytical normal with a finite height slope.
        const auto n=particle.center,tangent=unit(cross(n,{0,1,0}));
        constexpr f32 step=.001F;
        const auto plus=unit({n.x+tangent.x*step,n.y+tangent.y*step,n.z+tangent.z*step});
        const auto minus=unit({n.x-tangent.x*step,n.y-tangent.y*step,n.z-tangent.z*step});
        const auto slope=(clouds.sample(plus).radius-clouds.sample(minus).radius)/(2*step);
        const auto sample=clouds.sample(n);
        CHECK(std::abs(dot(sample.normal,tangent)/dot(sample.normal,n)+.6F*slope/sample.radius)<.01F);
    }
}
