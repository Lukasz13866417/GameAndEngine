#include "support/earth_assets.hpp"
#include "support/earth_clouds.hpp"
#include "support/earth_scene.hpp"
#include "support/mesh_frame.hpp"
#include "editor/editing_session.hpp"
#include "editor/animation.hpp"
#include "editor/preview_updates.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <chrono>
#include <iostream>

namespace {
using namespace vng;
namespace earth=example::earth;
namespace project=editor_example;
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
    REQUIRE(editing.undo());CHECK(editing.state().document.instances.size()==1);
    REQUIRE(editing.redo());CHECK(editing.state().document.instances.size()==2);
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
    project::EditingSession session{initial};project::PreviewUpdates updates;updates.add(1);updates.accepted(1,initial.document.revision);
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
