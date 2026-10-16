#include "nim/content/animation.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <unordered_set>

namespace nim {

// ---- transforms ----------------------------------------------------------------------------------
namespace {

using Q = std::array<float, 4>;
using V = std::array<float, 3>;

Q qmul(const Q& a, const Q& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Q qnormalize(Q q) {
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12f) return {0, 0, 0, 1};
    for (auto& c : q) c /= l;
    return q;
}

} // namespace

std::array<float, 3> rotate(const Q& q, const V& v) {
    // v + 2w (u x v) + 2 u x (u x v)
    const V u{q[0], q[1], q[2]};
    const V t{2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
    return {v[0] + q[3] * t[0] + (u[1] * t[2] - u[2] * t[1]), v[1] + q[3] * t[1] + (u[2] * t[0] - u[0] * t[2]),
            v[2] + q[3] * t[2] + (u[0] * t[1] - u[1] * t[0])};
}

std::array<float, 3> transform_point(const BoneTransform& t, const V& p) {
    V r = rotate(t.rotation, {p[0] * t.scale, p[1] * t.scale, p[2] * t.scale});
    return {r[0] + t.translation[0], r[1] + t.translation[1], r[2] + t.translation[2]};
}

BoneTransform operator*(const BoneTransform& parent, const BoneTransform& child) {
    BoneTransform r;
    r.rotation = qnormalize(qmul(parent.rotation, child.rotation));
    r.translation = transform_point(parent, child.translation);
    r.scale = parent.scale * child.scale;
    return r;
}

BoneTransform inverse(const BoneTransform& t) {
    BoneTransform r;
    r.rotation = {-t.rotation[0], -t.rotation[1], -t.rotation[2], t.rotation[3]};
    r.scale = t.scale != 0 ? 1.0f / t.scale : 1.0f;
    V p = rotate(r.rotation, t.translation);
    r.translation = {-p[0] * r.scale, -p[1] * r.scale, -p[2] * r.scale};
    return r;
}

// ---- skeleton ------------------------------------------------------------------------------------
namespace {

// Target of a resource pointer at `off` and the class hash stored just before it.
std::pair<size_t, uint32_t> resource_at(const TagReader& r, size_t off) {
    const uint64_t v = r.u64_at(off);
    if (v == 0 || off + v < 4 || off + v >= r.size()) return {0, 0};
    const size_t target = off + size_t(v);
    return {target, r.u32_at(target - 4)};
}

BoneTransform read_transform(const TagReader& r, size_t o) {
    BoneTransform t;
    for (int c = 0; c < 4; ++c) t.rotation[c] = r.f32_at(o + c * 4);
    for (int c = 0; c < 3; ++c) t.translation[c] = r.f32_at(o + 16 + c * 4);
    t.scale = r.f32_at(o + 28);
    if (!(t.scale > 0)) t.scale = 1;
    t.rotation = qnormalize(t.rotation);
    return t;
}

} // namespace

bool is_skeleton_component(PackageManager& pm, TagHash component) {
    const Entry* e = pm.entry(component);
    if (!e || e->type != 8 || e->reference != kClassComponent) return false;
    auto bytes = pm.read(component);
    TagReader r(bytes);
    return resource_at(r, 0x18).second == kClassSkeleton;
}

Skeleton load_skeleton(PackageManager& pm, TagHash component) {
    auto bytes = pm.read(component);
    TagReader r(bytes);
    auto [s, cls] = resource_at(r, 0x18);
    if (cls != kClassSkeleton) throw std::runtime_error(std::format("{} is not a skeleton component", component.str()));
    Skeleton sk;
    sk.component = component;
    auto nodes = r.vec_at(s + 0xB0);
    auto bind = r.vec_at(s + 0xC0);
    auto inv = r.vec_at(s + 0xD0);
    if (bind.count != nodes.count || inv.count != nodes.count) throw std::runtime_error("skeleton: array sizes differ");
    for (uint64_t i = 0; i < nodes.count; ++i) {
        const size_t o = nodes.data + size_t(i) * 16;
        sk.hashes.push_back(r.u32_at(o));
        int32_t parent = int32_t(r.u32_at(o + 4));
        if (parent >= int32_t(nodes.count)) parent = -1;
        sk.parents.push_back(parent);
        sk.bind.push_back(read_transform(r, bind.data + size_t(i) * 32));
        sk.inverse_bind.push_back(read_transform(r, inv.data + size_t(i) * 32));
    }
    sk.local_bind.resize(sk.size());
    for (size_t i = 0; i < sk.size(); ++i)
        sk.local_bind[i] = sk.parents[i] < 0 ? sk.bind[i] : inverse(sk.bind[size_t(sk.parents[i])]) * sk.bind[i];
    return sk;
}

AnimationRig load_animation_rig(PackageManager& pm, TagHash component) {
    auto bytes = pm.read(component);
    TagReader r(bytes);
    auto [a, cls] = resource_at(r, 0x18);
    if (cls != kClassAnimationRig) throw std::runtime_error(std::format("{} is not an animation rig component", component.str()));
    auto nodes = r.vec_at(a + 0xA8);
    auto pose = r.vec_at(a + 0xB8);
    auto to_skeleton = r.vec_at(a + 0xF0);
    if (pose.count != nodes.count || to_skeleton.count != nodes.count) throw std::runtime_error("rig: array sizes differ");
    AnimationRig rig;
    rig.component = component;
    for (uint64_t i = 0; i < nodes.count; ++i) {
        rig.skeleton_bone.push_back(r.u16_at(to_skeleton.data + size_t(i) * 2));
        const int16_t space = int16_t(r.u16_at(nodes.data + size_t(i) * 72 + 8));
        rig.object_space.push_back(space == 2 || space == 6);
        rig.default_pose.push_back(read_transform(r, pose.data + size_t(i) * 32));
    }
    return rig;
}

std::unordered_map<uint32_t, ModelRig> index_model_skeletons(
    PackageManager& pm, const std::unordered_map<uint32_t, DynamicModelMaterials>& materials) {
    std::unordered_map<uint32_t, std::vector<uint32_t>> models_of;  // component -> models it places
    for (const auto& [model, m] : materials) models_of[m.component.value].push_back(model);
    std::unordered_set<uint32_t> skeletons, rigs;
    std::vector<TagHash> entities;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8) continue;
            const TagHash tag = TagHash::make(info.pkg_id, uint16_t(i));
            if (e.reference == kClassEntity) entities.push_back(tag);
            if (e.reference != kClassComponent) continue;
            try {
                auto bytes = pm.read(tag);
                TagReader r(bytes);
                const uint32_t cls = resource_at(r, 0x18).second;
                if (cls == kClassSkeleton) skeletons.insert(tag.value);
                else if (cls == kClassAnimationRig) rigs.insert(tag.value);
            } catch (...) {
            }
        }
    }
    std::unordered_map<uint32_t, ModelRig> out;
    for (TagHash ent : entities) {
        try {
            auto bytes = pm.read(ent);
            TagReader r(bytes);
            auto comps = r.vec_at(0x08);
            ModelRig mr;
            std::vector<uint32_t> models;
            for (uint64_t k = 0; k < comps.count; ++k) {
                const uint32_t c = r.u32_at(comps.data + size_t(k) * 12);
                if (skeletons.count(c)) mr.skeleton = TagHash(c);
                if (rigs.count(c)) mr.rig = TagHash(c);
                if (auto f = models_of.find(c); f != models_of.end()) models.insert(models.end(), f->second.begin(), f->second.end());
            }
            if (mr.skeleton.is_none()) continue;
            for (uint32_t m : models) {
                auto [it, fresh] = out.emplace(m, mr);
                if (!fresh && it->second.rig.is_none()) it->second = mr;  // prefer an entity that also has a rig
            }
        } catch (...) {
        }
    }
    return out;
}

// ---- clips ---------------------------------------------------------------------------------------
const char* clip_codec_name(ClipCodec c) {
    switch (c) {
    case ClipCodec::Static: return "static";
    case ClipCodec::Sampled: return "sampled";
    case ClipCodec::Raw: return "raw";
    case ClipCodec::Curve: return "curve";
    }
    return "?";
}

namespace {

constexpr uint32_t kClassTrackSet = 0x8080B13B;    // constant / sampled tracks
constexpr uint32_t kClassRawTracks = 0x8080B13E;   // u16 per frame, per-component ranges
constexpr uint32_t kClassCurveTracks = 0x8080B13F; // key reduction + cubic segments
// Offsets of the cubic segment's inner control points, per nibble step, in normalized units
// (rotation components in [-1, 1], translation / scale in [0, 1] of the track range). Estimated
// from the data (docs/ARCHITECTURE.md 10.3); the corrections are below 1% of the range.
constexpr float kNibbleStep = 1.0f / 1024.0f;

std::vector<uint16_t> u16_vec(const TagReader& r, size_t off) {
    auto v = r.vec_at(off);
    std::vector<uint16_t> out(size_t(v.count));
    for (size_t i = 0; i < out.size(); ++i) out[i] = r.u16_at(v.data + i * 2);
    return out;
}
std::vector<uint8_t> u8_vec(const TagReader& r, size_t off) {
    auto v = r.vec_at(off);
    std::vector<uint8_t> out(size_t(v.count));
    for (size_t i = 0; i < out.size(); ++i) out[i] = r.u8_at(v.data + i);
    return out;
}
std::vector<float> f32_vec(const TagReader& r, size_t off) {
    auto v = r.vec_at(off);
    std::vector<float> out(size_t(v.count));
    for (size_t i = 0; i < out.size(); ++i) out[i] = r.f32_at(v.data + i * 4);
    return out;
}

float unorm16(uint16_t v) { return float(v) / 65535.0f; }
float snorm16(uint16_t v) { return std::max(float(int16_t(v)) / 32767.0f, -1.0f); }

struct BoneLists {
    std::vector<uint16_t> s, r, t;
};

// 8080B13B: u16 (3, scale count, rotation count, translation count), u32 frames, scale (extent,
// min), translation extent xyz, translation min xyz, Vec<u16> values; track-major sections
// [scale][rotation x4][translation x3]. Rotations are unorm16 mapped to [-1, 1].
void read_track_set(const TagReader& r, size_t a, const BoneLists& bones, AnimationClip& clip) {
    const uint32_t S = r.u16_at(a + 2), R = r.u16_at(a + 4), T = r.u16_at(a + 6);
    const uint32_t F = std::max(1u, r.u32_at(a + 0x10));
    const float se = r.f32_at(a + 0x14), sm = r.f32_at(a + 0x18);
    float te[3], tm[3];
    for (int c = 0; c < 3; ++c) te[c] = r.f32_at(a + 0x1C + c * 4), tm[c] = r.f32_at(a + 0x28 + c * 4);
    auto v = u16_vec(r, a + 0x38);
    if (v.size() != size_t(F) * (S + 4 * R + 3 * T) || bones.s.size() != S || bones.r.size() != R || bones.t.size() != T)
        throw std::runtime_error("clip: track set sizes do not match");
    size_t o = 0;
    for (uint32_t i = 0; i < S; ++i) {
        auto& tr = clip.scales.emplace_back();
        tr.bone = bones.s[i];
        for (uint32_t f = 0; f < F; ++f) tr.frames.push_back(sm + unorm16(v[o++]) * se);
    }
    for (uint32_t i = 0; i < R; ++i) {
        auto& tr = clip.rotations.emplace_back();
        tr.bone = bones.r[i];
        for (uint32_t f = 0; f < F; ++f, o += 4)
            tr.frames.push_back(qnormalize({unorm16(v[o]) * 2 - 1, unorm16(v[o + 1]) * 2 - 1, unorm16(v[o + 2]) * 2 - 1,
                                            unorm16(v[o + 3]) * 2 - 1}));
    }
    for (uint32_t i = 0; i < T; ++i) {
        auto& tr = clip.translations.emplace_back();
        tr.bone = bones.t[i];
        for (uint32_t f = 0; f < F; ++f, o += 3)
            tr.frames.push_back({tm[0] + unorm16(v[o]) * te[0], tm[1] + unorm16(v[o + 1]) * te[1], tm[2] + unorm16(v[o + 2]) * te[2]});
    }
}

// 8080B13E: u16 (2, S, R, T), u64 frames, Vec<u16> values (track-major, as above), Vec<f32>
// extents and Vec<f32> minimums, one per component in the order [scale][rotation][translation].
void read_raw_tracks(const TagReader& r, size_t a, const BoneLists& bones, AnimationClip& clip) {
    const uint32_t S = r.u16_at(a + 2), R = r.u16_at(a + 4), T = r.u16_at(a + 6);
    const uint32_t F = uint32_t(r.u64_at(a + 0x10));
    auto v = u16_vec(r, a + 0x18);
    auto ext = f32_vec(r, a + 0x28);
    auto mn = f32_vec(r, a + 0x38);
    const size_t comps = S + 4 * R + 3 * T;
    if (v.size() != size_t(F) * comps || ext.size() != comps || mn.size() != comps || bones.s.size() != S ||
        bones.r.size() != R || bones.t.size() != T)
        throw std::runtime_error("clip: raw track sizes do not match");
    auto val = [&](size_t i, size_t c) { return mn[c] + unorm16(v[i]) * ext[c]; };
    size_t o = 0;
    for (uint32_t i = 0; i < S; ++i) {
        auto& tr = clip.scales.emplace_back();
        tr.bone = bones.s[i];
        for (uint32_t f = 0; f < F; ++f) tr.frames.push_back(val(o++, i));
    }
    for (uint32_t i = 0; i < R; ++i) {
        auto& tr = clip.rotations.emplace_back();
        tr.bone = bones.r[i];
        const size_t c = S + 4 * i;
        for (uint32_t f = 0; f < F; ++f, o += 4)
            tr.frames.push_back(qnormalize({val(o, c), val(o + 1, c + 1), val(o + 2, c + 2), val(o + 3, c + 3)}));
    }
    for (uint32_t i = 0; i < T; ++i) {
        auto& tr = clip.translations.emplace_back();
        tr.bone = bones.t[i];
        const size_t c = S + 4 * R + 3 * i;
        for (uint32_t f = 0; f < F; ++f, o += 3) tr.frames.push_back({val(o, c), val(o + 1, c + 1), val(o + 2, c + 2)});
    }
}

// 8080B13F, per track (scale, rotation, translation order), entry e of Vec<u16> at 0x70:
//   e >= 0x8000: raw track at u16 offset ~e of Vec 0x10, `comps` values per frame;
//   otherwise a block at e - 1 of Vec 0x20: u16 segment-length offset (into Vec<u8> 0x30), u16
//   byte offset (into Vec<u8> 0x40), u16 segment count n, then n + 1 keys of 3 (1 for scale) u16.
//   Segment lengths sum to frames - 1. Each segment has one byte per channel: high nibble and low
//   nibble (0..14, 7 = none) displace the cubic's two inner control points. Rotations are (w, x, y)
//   snorm16 with |z| = sqrt(1 - w^2 - x^2 - y^2), negative when the low bit of x is set; their four
//   bytes are the x, y, w, z channels.
//   Scale and translation values are unorm16 inside per-track (extent 0x50, min 0x60) ranges.
std::vector<std::array<float, 4>> curve_track(const std::vector<uint16_t>& raw, const std::vector<uint16_t>& blocks,
                                              const std::vector<uint8_t>& lengths, const std::vector<uint8_t>& params,
                                              uint16_t entry, uint32_t frames, int comps, int channels, bool rotation) {
    auto norm = [&](uint16_t v) { return rotation ? snorm16(v) : unorm16(v); };
    auto channels_of = [&](const uint16_t* k) -> std::array<float, 4> {
        if (!rotation) return {norm(k[0]), comps > 1 ? norm(k[1]) : 0.0f, comps > 2 ? norm(k[2]) : 0.0f, 0};
        // Stored (w, x, y); |z| is derived and the low bit of x is set when z is negative.
        const float w = norm(k[0]), x = norm(k[1]), y = norm(k[2]);
        const float z = std::sqrt(std::max(0.0f, 1 - w * w - x * x - y * y));
        return {x, y, w, (k[1] & 1) ? -z : z};
    };
    std::vector<std::array<float, 4>> out;
    out.reserve(frames);
    if (entry >= 0x8000) {
        const size_t off = uint16_t(~entry);
        if (off + size_t(frames) * comps > raw.size()) throw std::runtime_error("clip: raw curve track out of range");
        for (uint32_t f = 0; f < frames; ++f) out.push_back(channels_of(&raw[off + size_t(f) * comps]));
        return out;
    }
    const size_t b = size_t(entry) - 1;
    if (entry == 0 || b + 3 > blocks.size()) throw std::runtime_error("clip: curve block out of range");
    const size_t len_off = blocks[b], par_off = blocks[b + 1], n = blocks[b + 2];
    if (b + 3 + (n + 1) * comps > blocks.size() || len_off + n > lengths.size() || par_off + n * channels > params.size())
        throw std::runtime_error("clip: curve segments out of range");
    const uint16_t* keys = &blocks[b + 3];
    for (size_t s = 0; s < n; ++s) {
        const auto p0 = channels_of(keys + s * comps), p1 = channels_of(keys + (s + 1) * comps);
        const uint32_t L = std::max<uint32_t>(1, lengths[len_off + s]);
        std::array<float, 4> c1{}, c2{};
        for (int c = 0; c < channels; ++c) {
            const uint8_t byte = params[par_off + s * channels + size_t(c)];
            const float d = p1[c] - p0[c];
            c1[c] = p0[c] + d / 3 + float(int(byte >> 4) - 7) * kNibbleStep;
            c2[c] = p1[c] - d / 3 - float(int(byte & 15) - 7) * kNibbleStep;
        }
        for (uint32_t f = 0; f < L; ++f) {
            const float t = float(f) / float(L), u = 1 - t;
            std::array<float, 4> v{};
            for (int c = 0; c < 4; ++c)
                v[c] = u * u * u * p0[c] + 3 * u * u * t * c1[c] + 3 * u * t * t * c2[c] + t * t * t * p1[c];
            out.push_back(v);
        }
    }
    out.push_back(channels_of(keys + n * comps));
    if (out.size() != frames) throw std::runtime_error(std::format("clip: curve has {} frames, expected {}", out.size(), frames));
    return out;
}

void read_curve_tracks(const TagReader& r, size_t a, const BoneLists& bones, uint32_t frames, AnimationClip& clip) {
    const uint32_t S = r.u16_at(a + 2), R = r.u16_at(a + 4), T = r.u16_at(a + 6);
    auto raw = u16_vec(r, a + 0x10);
    auto blocks = u16_vec(r, a + 0x20);
    auto lengths = u8_vec(r, a + 0x30);
    auto params = u8_vec(r, a + 0x40);
    auto ext = f32_vec(r, a + 0x50);
    auto mn = f32_vec(r, a + 0x60);
    auto entries = u16_vec(r, a + 0x70);
    if (entries.size() < S + R + T || ext.size() != S + T || mn.size() != S + T || bones.s.size() != S ||
        bones.r.size() != R || bones.t.size() != T)
        throw std::runtime_error("clip: curve track sizes do not match");
    for (uint32_t i = 0; i < S; ++i) {
        auto& tr = clip.scales.emplace_back();
        tr.bone = bones.s[i];
        for (const auto& v : curve_track(raw, blocks, lengths, params, entries[i], frames, 1, 1, false))
            tr.frames.push_back(mn[i] + v[0] * ext[i]);
    }
    for (uint32_t i = 0; i < R; ++i) {
        auto& tr = clip.rotations.emplace_back();
        tr.bone = bones.r[i];
        for (const auto& v : curve_track(raw, blocks, lengths, params, entries[S + i], frames, 3, 4, true))
            tr.frames.push_back(qnormalize({v[0], v[1], v[3], v[2]}));
    }
    for (uint32_t i = 0; i < T; ++i) {
        auto& tr = clip.translations.emplace_back();
        tr.bone = bones.t[i];
        const float e = ext[S + i], m = mn[S + i];
        for (const auto& v : curve_track(raw, blocks, lengths, params, entries[S + R + i], frames, 3, 3, false))
            tr.frames.push_back({m + v[0] * e, m + v[1] * e, m + v[2] * e});
    }
}

} // namespace

AnimationClip load_animation_clip(PackageManager& pm, TagHash tag) {
    auto bytes = pm.read(tag);
    TagReader r(bytes);
    AnimationClip clip;
    clip.tag = tag;
    clip.name_hash = r.u32_at(0x120);
    clip.frame_count = std::max<uint32_t>(1, r.u16_at(0x140));
    clip.bone_count = r.u16_at(0x142);
    BoneLists constant{u16_vec(r, 0xA8), u16_vec(r, 0xB8), u16_vec(r, 0xC8)};
    BoneLists animated{u16_vec(r, 0xD8), u16_vec(r, 0xE8), u16_vec(r, 0xF8)};
    // A few clips carry no tracks at all (only the 8080B146 block at 0x28).
    auto [c, ccls] = resource_at(r, 0x10);
    if (ccls == kClassTrackSet) read_track_set(r, c, constant, clip);
    else if (ccls != 0) throw std::runtime_error(std::format("{}: unknown constant block {:08X}", tag.str(), ccls));
    auto [a, acls] = resource_at(r, 0x18);
    if (acls == kClassTrackSet) {
        clip.codec = ClipCodec::Sampled;
        read_track_set(r, a, animated, clip);
    } else if (acls == kClassRawTracks) {
        clip.codec = ClipCodec::Raw;
        read_raw_tracks(r, a, animated, clip);
    } else if (acls == kClassCurveTracks) {
        clip.codec = ClipCodec::Curve;
        read_curve_tracks(r, a, animated, clip.frame_count, clip);
    } else if (acls != 0) {
        throw std::runtime_error(std::format("{}: unknown track block {:08X}", tag.str(), acls));
    }
    return clip;
}

std::vector<ClipInfo> index_animation_clips(PackageManager& pm) {
    std::vector<ClipInfo> out;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassAnimationClip) continue;
            const TagHash tag = TagHash::make(info.pkg_id, uint16_t(i));
            try {
                auto bytes = pm.read(tag);
                BinaryReader r(bytes);
                out.push_back({tag, r.u16_at(0x140), r.u16_at(0x142)});
            } catch (...) {
            }
        }
    }
    return out;
}

void sample_clip(const AnimationClip& clip, float frame, std::vector<BoneTransform>& local) {
    const float last = float(clip.frame_count - 1);
    frame = std::clamp(frame, 0.0f, last);
    auto pick = [&](size_t count, size_t& i0, size_t& i1, float& t) {
        if (count <= 1) {
            i0 = i1 = 0;
            t = 0;
            return;
        }
        const float f = std::min(frame, float(count - 1));
        i0 = size_t(f);
        i1 = std::min(i0 + 1, count - 1);
        t = f - float(i0);
    };
    size_t i0, i1;
    float t;
    for (const auto& tr : clip.rotations) {
        if (tr.bone >= local.size() || tr.frames.empty()) continue;
        pick(tr.frames.size(), i0, i1, t);
        Q a = tr.frames[i0], b = tr.frames[i1];
        const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const float s = dot < 0 ? -1.0f : 1.0f;
        local[tr.bone].rotation = qnormalize({a[0] + (b[0] * s - a[0]) * t, a[1] + (b[1] * s - a[1]) * t,
                                              a[2] + (b[2] * s - a[2]) * t, a[3] + (b[3] * s - a[3]) * t});
    }
    for (const auto& tr : clip.translations) {
        if (tr.bone >= local.size() || tr.frames.empty()) continue;
        pick(tr.frames.size(), i0, i1, t);
        for (int c = 0; c < 3; ++c)
            local[tr.bone].translation[c] = tr.frames[i0][c] + (tr.frames[i1][c] - tr.frames[i0][c]) * t;
    }
    for (const auto& tr : clip.scales) {
        if (tr.bone >= local.size() || tr.frames.empty()) continue;
        pick(tr.frames.size(), i0, i1, t);
        local[tr.bone].scale = tr.frames[i0] + (tr.frames[i1] - tr.frames[i0]) * t;
    }
}

bool is_additive(const AnimationRig& rig, const AnimationClip& clip) {
    size_t votes = 0, at_identity = 0;
    for (const auto& t : clip.rotations) {
        if (t.bone >= rig.size() || !rig.object_space[t.bone] || t.frames.empty()) continue;
        if (std::fabs(rig.default_pose[t.bone].rotation[3]) > 0.995f) continue;  // identity in the full pose too
        ++votes;
        if (std::fabs(t.frames[0][3]) > 0.995f) ++at_identity;
    }
    return votes > 0 && at_identity * 2 > votes;
}

void pose_skeleton(const Skeleton& sk, const AnimationRig& rig, const AnimationClip& clip, float frame,
                   std::vector<BoneTransform>& object) {
    std::vector<BoneTransform> pose = rig.default_pose;
    if (is_additive(rig, clip)) {
        std::vector<BoneTransform> delta(rig.size());
        sample_clip(clip, frame, delta);
        for (size_t i = 0; i < rig.size(); ++i) {
            pose[i].rotation = qnormalize(qmul(pose[i].rotation, delta[i].rotation));
            for (int k = 0; k < 3; ++k) pose[i].translation[k] += delta[i].translation[k];
        }
    } else {
        sample_clip(clip, frame, pose);
    }
    const size_t n = sk.size();
    std::vector<int32_t> rig_of(n, -1);
    for (size_t i = 0; i < rig.size(); ++i)
        if (rig.skeleton_bone[i] < n) rig_of[rig.skeleton_bone[i]] = int32_t(i);
    object.assign(n, BoneTransform{});
    std::vector<uint8_t> done(n, 0);
    // Parents precede children in Tiger skeletons; a few extra passes cover any that do not.
    for (size_t pass = 0; pass < 8; ++pass) {
        bool all = true;
        for (size_t b = 0; b < n; ++b) {
            if (done[b]) continue;
            const int32_t p = sk.parents[b];
            if (p >= 0 && !done[size_t(p)]) {
                all = false;
                continue;
            }
            const int32_t ri = rig_of[b];
            BoneTransform& o = object[b];
            if (p < 0) {
                o = sk.bind[b];
                if (ri >= 0) o.rotation = pose[size_t(ri)].rotation;
            } else {
                const BoneTransform& po = object[size_t(p)];
                o = po * sk.local_bind[b];
                if (ri >= 0) {
                    const BoneTransform& c = pose[size_t(ri)];
                    o.rotation = rig.object_space[size_t(ri)] ? c.rotation : qnormalize(qmul(po.rotation, c.rotation));
                    if (sk.parents[size_t(p)] < 0 && rig.object_space[size_t(ri)])
                        for (int k = 0; k < 3; ++k) o.translation[k] = sk.bind[b].translation[k] + c.translation[k];
                }
            }
            done[b] = 1;
        }
        if (all) break;
    }
}

// ---- skin weights --------------------------------------------------------------------------------
SkinWeights decode_skin_weights(const VertexBuffer& vb0, const VertexBuffer& skinning) {
    SkinWeights out;
    if (vb0.stride < 8 || vb0.data.empty()) return out;
    const size_t n = vb0.data.size() / vb0.stride;
    out.bones.resize(n, {0, 0, 0, 0});
    out.weights.resize(n, {1, 0, 0, 0});
    const auto& sk = skinning.data;
    for (size_t v = 0; v < n; ++v) {
        int16_t w;
        std::memcpy(&w, vb0.data.data() + v * vb0.stride + 6, 2);
        const int a = std::abs(int(w));
        if (a < 0x800) {
            out.bones[v] = {uint16_t(std::max(0, int(w))), 0, 0, 0};
            out.max_bone = std::max<uint32_t>(out.max_bone, out.bones[v][0]);
            continue;
        }
        const bool four = w < 0;
        const size_t element = size_t(a - 0x800) * 8 + ((v << (four ? 1 : 0)) & 7);
        const size_t off = element * 4;
        if (off + (four ? 8 : 4) > sk.size()) continue;
        const uint8_t* e = sk.data() + off;
        out.bones[v] = {e[0], e[1], 0, 0};
        out.weights[v] = {e[2] / 255.0f, e[3] / 255.0f, 0, 0};
        if (four) {
            out.bones[v][2] = e[4];
            out.bones[v][3] = e[5];
            out.weights[v][2] = e[6] / 255.0f;
            out.weights[v][3] = e[7] / 255.0f;
        }
        const float sum = out.weights[v][0] + out.weights[v][1] + out.weights[v][2] + out.weights[v][3];
        if (sum > 0)
            for (auto& x : out.weights[v]) x /= sum;
        for (int k = 0; k < 4; ++k)
            if (out.weights[v][k] > 0) out.max_bone = std::max<uint32_t>(out.max_bone, out.bones[v][k]);
    }
    return out;
}

} // namespace nim
