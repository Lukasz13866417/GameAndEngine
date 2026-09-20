#pragma once
#include <vng/content/vmesh.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <optional>
#include <span>

namespace example::mesh_frame {
// Baked geometry stays an ordinary mesh. Procedural editors also retain its
// authoring frame so later part edits/rebuilds use the same transformed space.
using namespace vng;
inline constexpr std::string_view metadata_key="editor/mesh-frame";
inline content::Diagnostic error(std::string message) { content::Diagnostic d;d.message=std::move(message);return d; }
inline Vec3 vector(const Mat4& m,Vec3 p) {
    Vec3 out{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)out[r]+=m[c][r]*p[c];return out;
}
inline Vec3 point(const Mat4& m,Vec3 p) {
    auto out=vector(m,p);for(unsigned c=0;c<3;++c)out[c]+=m[3][c];return out;
}
inline Mat4 compose(const Mat4& a,const Mat4& b) {
    Mat4 out{};for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)
        for(unsigned k=0;k<4;++k)out[c][r]+=a[k][r]*b[c][k];
    return out;
}
inline content::Result<Mat4> inverse(const Mat4& m) {
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)
        if(!std::isfinite(m[c][r]))return std::unexpected(error("Mesh frame must be finite"));
    if(m[0][3]!=0 || m[1][3]!=0 || m[2][3]!=0 || m[3][3]!=1)
        return std::unexpected(error("Mesh frame must be affine"));
    double cofactors[3][3]{};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
        cofactors[r][c]=double(m[(r+1)%3][(c+1)%3])*m[(r+2)%3][(c+2)%3]-
            double(m[(r+2)%3][(c+1)%3])*m[(r+1)%3][(c+2)%3];
    double determinant{};for(unsigned c=0;c<3;++c)determinant+=double(m[c][0])*cofactors[c][0];
    if(!std::isfinite(determinant)||determinant<=0)
        return std::unexpected(error("Mesh frame needs a positive, nonsingular scale"));
    auto out=Mat4::identity();
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)out[c][r]=f32(cofactors[r][c]/determinant);
    const auto offset=vector(out,{m[3][0],m[3][1],m[3][2]});
    out[3]={-offset.x,-offset.y,-offset.z,1};
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)
        if(!std::isfinite(out[c][r]))return std::unexpected(error("Mesh frame inverse overflow"));
    return out;
}
inline Vec3 normal(const Mat4& inverse,Vec3 n) {
    Vec3 out{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)out[r]+=inverse[r][c]*n[c];
    const auto length=std::hypot(out.x,out.y,out.z);
    if(length>0)for(unsigned c=0;c<3;++c)out[c]/=length;
    return out;
}
inline content::Result<Mat4> read(const content::vmesh::Document& d) {
    const auto found=d.metadata.find(std::string(metadata_key));
    if(found==d.metadata.end())return Mat4::identity();
    std::istringstream input(found->second);input.imbue(std::locale::classic());
    Mat4 m;
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)
        if(!(input>>m[c][r]))return std::unexpected(error("Invalid mesh authoring frame"));
    std::string extra;if(input>>extra)return std::unexpected(error("Extra mesh authoring frame values"));
    if(auto checked=inverse(m);!checked)return std::unexpected(checked.error());
    return m;
}
inline void write(content::vmesh::Document& d,const Mat4& m) {
    std::string value;
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r) {
        char bytes[64];const auto converted=std::to_chars(bytes,bytes+64,m[c][r]);
        if(!value.empty())value+=' ';
        value.append(bytes,converted.ptr);
    }
    d.metadata[std::string(metadata_key)]=std::move(value);
}
// Transform only conventional position/normal fields; preserve custom fields,
// topology and ownership. The caller decides whether to record the new frame.
inline content::Result<void> bake(content::vmesh::Document& d,const Mat4& m,
    std::optional<std::span<const u32>> selected={}) {
    if(m==Mat4::identity())return {};
    auto inv=inverse(m);if(!inv)return std::unexpected(inv.error());
    bool translation_only=true;
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
        translation_only&=m[c][r]==(r==c?1.F:0.F);
    for(auto& field:d.vertex_fields)if(field.name=="position" || field.name=="normal") {
        if(field.name=="normal" && translation_only)continue;
        if(field.type!=content::vmesh::FieldType{content::vmesh::ScalarType::Float32,3}) {
            if(field.name=="normal")continue; // Same optional-normal convention as the mesh renderer.
            return std::unexpected(error("Whole mesh transform needs float3 positions"));
        }
        auto& values=std::get<std::vector<f32>>(field.values);
        const auto update=[&](std::size_t vertex) {
            const auto i=vertex*3;
            const Vec3 v{values[i],values[i+1],values[i+2]};
            const auto transformed=field.name=="position"?point(m,v):normal(*inv,v);
            for(unsigned c=0;c<3;++c)values[i+c]=transformed[c];
        };
        if(selected)for(auto vertex:*selected)update(vertex);
        else for(std::size_t vertex=0;vertex<d.vertex_count;++vertex)update(vertex);
    }
    return {};
}
inline content::Result<content::vmesh::Document> transformed(const content::vmesh::Document& source,const Mat4& m) {
    if(m==Mat4::identity())return source;
    auto previous=read(source);if(!previous)return std::unexpected(previous.error());
    auto result=source;
    if(auto baked=bake(result,m);!baked)return std::unexpected(baked.error());
    const auto frame=compose(m,*previous);
    if(auto checked=inverse(frame);!checked)return std::unexpected(checked.error());
    write(result,frame);return result;
}
}
