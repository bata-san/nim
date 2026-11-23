#pragma once
#include "nim/content/tfx.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
// 3x4 rows (rotation * scale | translation), as map placements.
std::array<float, 12> transform_rows(const BoneTransform& t);

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
// 0xA8 Vec of 72-byte nodes (i16 at +8: 0/1 parent-relative, 2/6 object space; i16 +0x2C/+0x2E an
// IK chain's end and root skeleton bones, root -1 for none), 0xB8 a default
// pose (32 B transforms, rig order), 0xE0 skeleton -> rig and 0xF0 rig -> skeleton (u16).
struct AnimationBoneGroup {
    uint32_t hash = 0, bones = 0;
    bool operator==(const AnimationBoneGroup&) const = default;
};

struct AnimationRig {
    TagHash component;
    std::vector<uint16_t> skeleton_bone;      // rig bone -> skeleton bone
    std::vector<uint8_t> object_space;        // rig bone: rotation is object space, not parent-relative
    std::vector<uint8_t> bind_relative;      // mode 1 with identity rig rest, a bind-relative candidate
    std::vector<BoneTransform> default_pose;  // rig order
    std::vector<AnimationBoneGroup> bone_groups;
    std::vector<uint8_t> node_layout;         // full authored node records for layout comparisons
    // Per rig node: the skeleton bone at the root of the IK chain that ends at this node's bone, or
    // -1 (node +0x2C i16 chain end = its own bone, +0x2E i16 chain root). The UESC robots chain
    // thigh -> knee -> ankle and upper arm -> elbow -> wrist; the node's translation track is the
    // chain's target.
    std::vector<int16_t> ik_root;
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
// Animation banks explicitly referenced by the entities that own a model. Sorted by tag.
struct ModelMotionBank {
    std::vector<TagHash> clips;
    std::vector<TagHash> shell_clips;
};
using ModelMotionIndex = std::unordered_map<uint32_t, ModelMotionBank>;
ModelRigIndex index_model_skeletons(PackageManager& pm, const std::unordered_map<uint32_t, DynamicModelMaterials>& materials,
                                   ModelAssemblyIndex* assemblies = nullptr, ModelMotionIndex* motions = nullptr);
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

// A clip's event track (docs/PROGRESS.md 3.90): header +0x160 Vec of relative pointers to
// polymorphic events {u32 class, u16 frame, u16 kind, u32 value, ...} (each pointer lands just
// past the class field):
//   8080AE32 kind 5: +0x0C u32 name, the key of the entity's 8080AE30 sound tables;
//   8080AE33 kind 4: value = key of the entity's 8080B4D1 dialogue tables (vocal barks);
//   8080AE3B kind 2: value = a foot / contact id (0x1E/0x1F, 0x0A/0x0B, ...: footplants, not decoded);
//   8080AE34 kind 3: value = a condition hash (not decoded).
struct ClipEvent {
    uint32_t frame = 0;
    uint16_t kind = 0;
    uint32_t cls = 0, value = 0, name = 0;
};

struct AnimationClip {
    TagHash tag;
    std::vector<ClipEvent> events;
    uint32_t name_hash = 0;
    uint32_t frame_count = 1;
    uint32_t bone_count = 0;
    std::vector<AnimationBoneGroup> bone_groups;
    ClipCodec codec = ClipCodec::Static;
    // Header +0x70: the body's travel over the whole clip in object space (x forward, y left):
    // quaternion, translation, then 1. A locomotion cycle stores its stride (assault walk 1.72 m,
    // run 3.2 m forward), a turn in place its yaw (+-60/90/100 degrees); most other clips identity.
    BoneTransform root_motion;
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
    uint32_t name_hash = 0;
    uint16_t shared_bones = 0;  // preview transfer only: keep this many leading rig nodes
    bool shell_specific = false;  // selected by this shell's branch of the authored animation graph
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
// space (object-space rotations are absolute); bone offsets stay those of the bind pose, except that
// bones directly under the root move by the clip translation (object space: absolute when the rig
// default sits nearer the bind position than the origin, else relative to bind). Root motion is
// dropped. Two-bone IK chains whose target is a ground-level object-space position (the robots'
// feet) are then solved onto the target, keeping the FK bend plane.
void pose_skeleton(const Skeleton& skeleton, const AnimationRig& rig, const AnimationClip& clip, float frame,
                   std::vector<BoneTransform>& object, bool authored_local_offsets = false);

// The clips of an entity's motion banks (components whose resource is 80803936 name a bank
// 80803BE9 at resource +0xE8; bank order is the animation graph's clip index). In bank order,
// duplicates dropped.
std::vector<TagHash> entity_motion_clips(PackageManager& pm, TagHash entity);
// The clips of one motion bank (80803BE9; rows of 8080AE00 wide hashes), none for unresolved rows.
std::vector<TagHash> motion_bank_clips(PackageManager& pm, TagHash bank);

// What an entity's animation components say about its clips (the UESC robots; 3.91):
// - the motion component (resource 80803936): +0xE8 bank, +0xF0 the character's tag in the
//   character category of the tag tree 8080ACBF (uesc_recruit = fnv1("uesc_recruit"), ...), +0xF4
//   the action graph 80803950, whose named nodes ("melee", "fire_1", "grenade_launch", "idle", ...)
//   pick clips by tags for actions;
// - the locomotion component (resource 8080397B): +0x170 bank, +0x178 the named parameter table
//   80803B54 (rows {u32 name, u32 kind, rel pointer}; kind 0x16 = clips 80803B5B whose +0x00
//   Vec<80803C5E> holds {u16 bank index, u16}), e.g. "idle_slow".
struct EntityMotionData {
    TagHash bank, graph, parameters;
    uint32_t character_tag = 0;
    std::vector<TagHash> bank_clips;                           // bank order
    std::unordered_map<uint32_t, std::vector<TagHash>> named;  // parameter table: name -> clips
    std::unordered_set<uint32_t> parameter_clips;              // every clip the parameter table holds
    std::unordered_set<uint32_t> action_clips;                 // every clip a node of the action graph plays
};
std::optional<EntityMotionData> entity_motion_data(PackageManager& pm, TagHash entity);
// Rebinds a clip authored for another rig of the same family by bone group: each clip group
// (AnimationClip::bone_groups, hash and node count, in track order) whose hash and count a group of
// `rig` has moves onto that group's nodes; tracks of groups the rig lacks are dropped and those rig
// nodes keep the default pose. The UESC robots' rigs all start with group 522AAFD7 (62 nodes:
// body, limbs, IK targets), so the shared bank's clips of one robot play on the others. None when
// the clip's first group is not in the rig.
std::optional<AnimationClip> map_clip_groups(const AnimationClip& clip, const AnimationRig& rig);
// Root travel of a clip: the animated translation track of the lowest-numbered bone that has one,
// first to last frame. speed is horizontal metres per second at `fps` (推定: the clip rate is not
// decoded; callers pass their estimate).
struct ClipRootMotion {
    TagHash clip;
    uint32_t frames = 0;
    uint16_t bone = 0;
    float seconds = 0;
    std::array<float, 3> delta{};
    float speed = 0;
};
ClipRootMotion measure_clip_root_motion(const AnimationClip& clip, float fps);
// Marathon's locomotion clips are in place (no root travel), so the travel a clip stands for is
// read from its feet (推定): the two ankle bones (bind height 0.08..0.25 m, mirrored sideways),
// posed over every frame. During stance (the lower half of a foot's height range) the foot slides
// opposite to the travel; speed = mean stance slide speed of both feet, direction = the opposite of
// that slide in the object's xy. cyclic: first and last pose match within 3 cm.
std::optional<std::array<int, 2>> find_feet(const Skeleton& skeleton);
struct ClipGait {
    TagHash clip;
    float seconds = 0;
    float speed = 0;                     // m/s, horizontal
    std::array<float, 2> direction{};    // unit xy in object space (x = forward), zero when standing
    float lift = 0;                      // max foot height range, metres
    bool cyclic = false;
    bool authored = false;               // speed and direction from the clip's root motion, not its feet
    float yaw = 0;                       // radians the root motion turns (+ = left, about +z)
};
// A clip with root motion (AnimationClip::root_motion, 2 cm or more of travel) takes its speed and
// direction from it: travel / duration at `fps`. The feet still give lift and cyclic.
ClipGait measure_clip_gait(const Skeleton& skeleton, const AnimationRig& rig, const AnimationClip& clip,
                           std::array<int, 2> feet, float fps);

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
