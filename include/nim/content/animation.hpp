#pragma once
#include "nim/content/tfx.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nim {

// Skeletons, animation clips and skin weights (docs/ARCHITECTURE.md, section 10).

constexpr uint32_t kClassEntity = 0x8080BAAD;
constexpr uint32_t kClassSkeleton = 0x80809FB7;       // resource (0x18) of a component tag
constexpr uint32_t kClassAnimationClip = 0x8080AE01;
constexpr uint32_t kClassAnimationRig = 0x8080AD7E;   // resource (0x18) of a component tag

// Rotation (x, y, z, w), translation, uniform scale.
struct BoneTransform {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> translation{};
    float scale = 1;
    bool operator==(const BoneTransform&) const = default;
};
BoneTransform operator*(const BoneTransform& parent, const BoneTransform& child);
BoneTransform inverse(const BoneTransform& t);
std::array<float, 3> transform_point(const BoneTransform& t, const std::array<float, 3>& p);
std::array<float, 3> rotate(const std::array<float, 4>& q, const std::array<float, 3>& v);

// ---- Skeleton ------------------------------------------------------------------------------------
// S80809FB7 (inside a component tag): 0xB0 nodes (16 B: hash, parent, first child, next sibling),
// 0xC0 object-space bind transforms, 0xD0 their inverses (32 B: quaternion, translation + scale).
struct Skeleton {
    TagHash component;
    std::vector<uint32_t> hashes;
    std::vector<int32_t> parents;
    std::vector<BoneTransform> bind;          // object space
    std::vector<BoneTransform> inverse_bind;  // object space -> bone space
    std::vector<BoneTransform> local_bind;    // parent-relative (derived)
    size_t size() const { return parents.size(); }
};
bool is_skeleton_component(PackageManager& pm, TagHash component);
Skeleton load_skeleton(PackageManager& pm, TagHash component);

// ---- Animation rig -------------------------------------------------------------------------------
// S8080AD7E (inside a component tag next to the skeleton in the entity). Clips address bones in
// rig order; the rig maps each to a skeleton bone and says in which space the clip's rotation is:
// 0xA8 Vec of 72-byte nodes (i16 at +8: 0/1 parent-relative, 2/6 object space), 0xB8 a default
// pose (32 B transforms, rig order), 0xE0 skeleton -> rig and 0xF0 rig -> skeleton (u16).
struct AnimationBoneGroup {
    uint32_t hash = 0, bones = 0;
    bool operator==(const AnimationBoneGroup&) const = default;
};

struct AnimationRig {
    TagHash component;
    std::vector<uint16_t> skeleton_bone;      // rig bone -> skeleton bone
    std::vector<uint8_t> object_space;        // rig bone: rotation is object space, not parent-relative
    std::vector<uint8_t> bind_relative;      // local mode with identity rig rest transform
    std::vector<BoneTransform> default_pose;  // rig order
    std::vector<AnimationBoneGroup> bone_groups;
    std::vector<uint8_t> node_layout;         // full authored node records for layout comparisons
    size_t size() const { return skeleton_bone.size(); }
};
AnimationRig load_animation_rig(PackageManager& pm, TagHash component);

// Model tag -> skeleton and rig components, from the model's entity or an unambiguous parent.
struct ModelRig {
    TagHash skeleton, rig;
    bool operator==(const ModelRig&) const = default;
};
using ModelRigIndex = std::unordered_map<uint32_t, ModelRig>;
using ModelAssemblyIndex = std::unordered_map<uint32_t, std::vector<TagHash>>;
ModelRigIndex index_model_skeletons(PackageManager& pm, const std::unordered_map<uint32_t, DynamicModelMaterials>& materials,
                                   ModelAssemblyIndex* assemblies = nullptr);
// For an assembled part missing a direct binding, require a unique skeleton and rig layout
// of the declared size which shares several authored bind joints with another part.
std::optional<ModelRig> find_assembly_rig(PackageManager& pm, const ModelRigIndex& models, uint32_t bones,
                                           const Skeleton& other);
// For a skinned model with no skeleton reachable from its entity (outfits, bodies and faces the
// game picks at run time), the skeleton + rig of `bones` bones whose bind joints its skin fits:
// the mean distance of each vertex to the bone segment it is mostly weighted to. Candidates with
// equivalent bind poses prefer a rig with clips. None when nothing fits within 20 cm or another
// skeleton fits nearly as well (within 1.5x). An estimate.
struct ClipInfo;
struct SkinFit {
    ModelRig binding;
    float error = 0;  // metres
};
std::optional<SkinFit> fit_skin_to_skeleton(PackageManager& pm, const ModelRigIndex& models, uint32_t bones,
                                            std::span<const std::array<float, 3>> positions,
                                            std::span<const std::array<uint16_t, 4>> vertex_bones,
                                            std::span<const std::array<float, 4>> vertex_weights,
                                            std::span<const ClipInfo> clips = {},
                                            std::vector<SkinFit>* ranking = nullptr);
// Each secondary bone follows the corresponding source bind joint or its own nearest
// ancestor with a matching bind transform. Unmatched bones stay in bind pose (-1).
std::vector<int32_t> assembly_pose_map(const Skeleton& source, const Skeleton& target);

// ---- Animation clips (8080AE01) -----------------------------------------------------------------
// Constant tracks (8080B13B at 0x10) plus one animated block at 0x18:
//   8080B13B "sampled": every frame in the constant encoding;
//   8080B13E "raw":     u16 per frame and component, per-component (min, extent);
//   8080B13F "curve":   per-track key reduction with cubic segments, or raw u16 tracks.
enum class ClipCodec : uint8_t { Static, Sampled, Raw, Curve };
const char* clip_codec_name(ClipCodec c);

struct AnimationClip {
    TagHash tag;
    uint32_t name_hash = 0;
    uint32_t frame_count = 1;
    uint32_t bone_count = 0;
    std::vector<AnimationBoneGroup> bone_groups;
    ClipCodec codec = ClipCodec::Static;
    template <typename T> struct Track { uint16_t bone = 0; std::vector<T> frames; };  // one entry = constant
    std::vector<Track<std::array<float, 4>>> rotations;
    std::vector<Track<std::array<float, 3>>> translations;
    std::vector<Track<float>> scales;
    bool has_tracks() const { return !rotations.empty() || !translations.empty() || !scales.empty(); }
};
AnimationClip load_animation_clip(PackageManager& pm, TagHash tag);

struct ClipInfo {
    TagHash tag;
    uint16_t frames = 0, bones = 0;
    std::vector<AnimationBoneGroup> bone_groups;
};
// Every clip in the install with its frame and bone counts and ordered bone groups.
std::vector<ClipInfo> index_animation_clips(PackageManager& pm);
// Prefer the rig's own groups. If the install has none, use groups from a rig with identical
// node records, default pose, and skeleton mapping.
std::vector<ClipInfo> compatible_animation_clips(PackageManager& pm, const ModelRigIndex& models,
                                                const AnimationRig& rig, std::span<const ClipInfo> clips);

// Overwrites the bones the clip animates in `pose` (indexed by clip bone = rig bone) with the
// pose at `frame` (fractional frames interpolate; quaternions are blended with nlerp).
void sample_clip(const AnimationClip& clip, float frame, std::vector<BoneTransform>& pose);
// Additive clips hold deltas on the rig's default pose: their object-space bones sit at identity
// where a full pose never does.
bool is_additive(const AnimationRig& rig, const AnimationClip& clip);
// Object-space skeleton pose for a clip of the rig at `frame` (additive clips are applied to the
// rig's default pose). Rotations follow the rig's per-bone
// space; bone offsets stay those of the bind pose, except that bones directly under the root move
// by the clip translation (object space, relative to bind). Root motion is dropped.
void pose_skeleton(const Skeleton& skeleton, const AnimationRig& rig, const AnimationClip& clip, float frame,
                   std::vector<BoneTransform>& object);

// ---- Skin weights ---------------------------------------------------------------------------------
// The game's compute_skinning reads the bone binding from vb0's position w (s16): |w| < 0x800 is
// a single bone; otherwise (|w| - 0x800) is a 32-byte chunk of the skinning buffer (40/4, read as
// R8G8B8A8_UINT): w > 0 holds 8 vertices x (bone0, bone1, weight0, weight1), w < 0 holds 4 vertices x
// two such entries.
struct SkinWeights {
    std::vector<std::array<uint16_t, 4>> bones;
    std::vector<std::array<float, 4>> weights;
    uint32_t max_bone = 0;
};
SkinWeights decode_skin_weights(const VertexBuffer& vb0, const VertexBuffer& skinning);

} // namespace nim
