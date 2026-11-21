#include "nim/content/animation.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
using namespace nim;
namespace fs = std::filesystem;
template<class T> T get(const std::vector<uint8_t>& v, size_t at) {
    if (at + sizeof(T) > v.size()) throw std::runtime_error("buffer truncated");
    T a; std::memcpy(&a, v.data() + at, sizeof(a)); return a;
}
template<class R> void arr(std::ostream& o, const R& r) {
    const char* sep = "[";
    for (const auto& v : r) { o << sep << +v; sep = ","; }
    o << (r.empty() ? "[]" : "]");
}
void transform(std::ostream& o, const BoneTransform& t) {
    o << '['; arr(o, t.rotation); o << ','; arr(o, t.translation); o << ',' << t.scale << ']';
}
void dump_texture(const Texture& t, const fs::path& path) {
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(t.data.data()), t.data.size());
    if (!o) throw std::runtime_error("texture write");
}
int main(int argc, char** argv) try {
    if (argc != 5) throw std::runtime_error("usage: extract MODEL SKELETON RIG CLIP");
    auto tag = [&](int i) { return uint32_t(std::stoul(argv[i], nullptr, 16)); };
    PackageManager pm(PackageManager::find_install().value());
    pm.build_hash64_index();
    const auto all = index_dynamic_model_materials(pm);
    const auto& mat = all.at(tag(1));
    const auto model = load_dynamic_model(pm, TagHash(tag(1)));
    if (model.meshes.size() != 1) throw std::runtime_error("unexpected number of meshes " + std::to_string(model.meshes.size()));
    const auto& mesh = model.meshes.front();
    const auto v = load_vertex_buffer(pm, mesh.vertex0), uv = load_vertex_buffer(pm, mesh.vertex1);
    const auto ix = load_index_buffer(pm, mesh.index);
    const auto skin = decode_skin_weights(v, load_vertex_buffer(pm, mesh.skinning));
    const auto sk = load_skeleton(pm, TagHash(tag(2)));
    const auto rig = load_animation_rig(pm, TagHash(tag(3)));
    const auto clip = load_animation_clip(pm, TagHash(tag(4)));
    if (v.stride != 24 || uv.stride != 4 || v.data.size()/v.stride != uv.data.size()/uv.stride || ix.is_32bit ||
        sk.size() != rig.size() || rig.size() != clip.bone_count || is_additive(rig, clip))
        throw std::runtime_error(std::string("unexpected mesh/clip layout: v stride ") + std::to_string(v.stride) + " uv stride " + std::to_string(uv.stride) + " 32bit " + std::to_string(ix.is_32bit) + " skel " + std::to_string(sk.size()) + " rig " + std::to_string(rig.size()) + " clip bones " + std::to_string(clip.bone_count) + " additive " + std::to_string(is_additive(rig, clip)));
    const auto snorm = [](int16_t x) { return std::max(-1.f, float(x)/32767.f); };
    std::ofstream o("asset.json");
    o << std::setprecision(9) << "{\"vertices\":[";
    for (size_t i=0; i<skin.bones.size(); ++i) {
        if (i) o << ',';
        std::array<float,3> p{}, n{};
        std::array<float,4> tangent{};
        std::array<float,2> tex{};
        for (size_t k=0; k<3; ++k) {
            p[k] = snorm(get<int16_t>(v.data, i*24 + k*2)) * model.model_scale[k] + model.model_offset[k];
            n[k] = snorm(get<int16_t>(v.data, i*24 + 8 + k*2));
        }
        for (size_t k=0; k<4; ++k) tangent[k] = snorm(get<int16_t>(v.data, i*24 + 16 + k*2));
        for (size_t k=0; k<2; ++k)
            tex[k] = snorm(get<int16_t>(uv.data, i*4 + k*2)) * model.texcoord_scale[k] + model.texcoord_offset[k];
        o << '['; arr(o,p); o << ','; arr(o,n); o << ','; arr(o,tex); o << ','; arr(o,tangent); o << ',';
        arr(o,skin.bones[i]); o << ','; arr(o,skin.weights[i]); o << ']';
    }
    o << "],\"parts\":[";
    fs::create_directories("raw");
    std::set<uint32_t> textures;
    const char* sep = "";
    for (unsigned stage : {0u, 8u}) {
        for (auto j = mesh.part_range[stage]; j < mesh.part_range[stage+1]; ++j) {
            const auto& part = mesh.parts.at(j);
            if (!(part.lod_category & 1)) continue;
            if (part.primitive_type != 5) throw std::runtime_error("unexpected topology");
            auto tech = mat.technique(part.variant_shader_index, mat.default_permutation);
            if (tech.is_none()) tech = part.technique;
            const auto technique = load_technique(pm, tech);
            o << sep << "{\"part\":"<<j<<",\"stage\":"<<stage<<",\"technique\":\""<<tech.str()<<"\",\"variant\":"<<part.variant_shader_index<<",\"indices\":[";
            sep = ",";
            for (uint32_t k=0; k<part.index_count; ++k) {
                if (k) o << ',';
                o << get<uint16_t>(ix.data, (part.index_start + k)*2);
            }
            o << "],\"textures\":{";
            const char* ss = "";
            for (auto stage_id : {ShaderStage::Vertex, ShaderStage::Pixel}) {
                const auto& program = technique.stage(stage_id);
                for (const auto& binding : program.textures) {
                    if (binding.texture.is_none()) continue;
                    o << ss << '"' << (stage_id == ShaderStage::Vertex ? "vs:" : "ps:") << binding.slot << "\":\"" << binding.texture.str() << '"';
                    ss = ",";
                    textures.insert(binding.texture.value);
                }
            }
            o << "}}";
        }
    }
    o << "],\"texture_info\":{";
    sep = "";
    for (auto value : textures) {
        const auto tag = TagHash(value);
        const auto t = load_texture(pm, tag);
        dump_texture(t, fs::path("raw") / (tag.str() + ".bin"));
        o << sep << '"' << tag.str() << "\":{\"format\":" << t.dxgi_format << ",\"width\":" << t.width
          << ",\"height\":" << t.height << ",\"depth\":" << t.depth << ",\"array\":" << t.array_size
          << ",\"mips\":" << t.mip_count << ",\"tiles\":" << t.tile_count << ",\"tile_uv\":";
        arr(o,t.tile_uv); o << ",\"bytes\":" << t.data.size() << '}';
        sep = ",";
    }
    o << "},\"bones\":[";
    for (size_t i=0; i<sk.size(); ++i) {
        if (i) o << ',';
        o << "{\"name\":\"" << TagHash(sk.hashes[i]).str() << "\",\"parent\":" << sk.parents[i] << ",\"bind\":";
        transform(o,sk.bind[i]); o << '}';
    }
    o << "],\"frames\":[";
    for (uint32_t f=0; f<clip.frame_count; ++f) {
        if (f) o << ',';
        auto pose = rig.default_pose;
        sample_clip(clip,float(f),pose);
        std::vector<BoneTransform> local(sk.size());
        for (size_t r=0; r<rig.size(); ++r) {
            if (rig.object_space[r] || rig.skeleton_bone[r] >= sk.size()) throw std::runtime_error("unexpected rig mapping");
            local[rig.skeleton_bone[r]] = pose[r];
        }
        o << '[';
        for (size_t i=0; i<sk.size(); ++i) {
            if (i) o << ',';
            transform(o,local[i]);
        }
        o << ']';
    }
    o << "],\"permutation\":" << mat.default_permutation << ",\"permutation_count\":" << mat.permutation_count << '}';
    if (!o) throw std::runtime_error("asset write");
    return 0;
} catch(const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
}

