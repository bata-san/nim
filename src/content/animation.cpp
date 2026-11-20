#include "nim/content/animation.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <set>
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

std::vector<AnimationBoneGroup> read_bone_groups(const TagReader& r, size_t offset) {
    const auto groups = r.vec_at(offset);
    std::vector<AnimationBoneGroup> result;
    if (groups.element_class != 0x8080AE7F) return result;
    for (uint64_t i = 0; i < groups.count; ++i) {
        const size_t row = groups.data + size_t(i) * 8;
        result.push_back({r.u32_at(row), r.u32_at(row + 4)});
    }
    return result;
}

// S808032B3 holds named child entities (S808032BA, 0x40-byte rows).
std::vector<TagHash> read_child_entities(PackageManager& pm, const TagReader& r, size_t resource,
                                        std::vector<TagHash>* colocated) {
    const auto children = r.vec_at(resource + 0x118);
    std::vector<TagHash> result;
    if (children.element_class != 0x808032BA) return result;
    for (uint64_t i = 0; i < children.count; ++i) {
        const size_t row = children.data + size_t(i) * 0x40;
        const auto child = resolve_wide_hash(pm, r.u32_at(row + 0x10), r.u64_at(row + 0x18));
        if (!child) continue;
        const Entry* entry = pm.entry(*child);
        if (!entry || entry->type != 8 || entry->reference != kClassEntity) continue;
        result.push_back(*child);
        // Child rows with no attachment or placement payload are already in the owning
        // model's space. Leave children whose placement depends on a link out of this preview.
        if (colocated && r.u32_at(row + 4) == 0 && r.u64_at(row + 8) == 0 &&
            r.u32_at(row + 0x14) == 0 && r.u64_at(row + 0x28) == 0 &&
            r.u64_at(row + 0x30) == 0 && r.u64_at(row + 0x38) == 0) colocated->push_back(*child);
    }
    return result;
}

// S8080A313 groups optional equipment parts. A slot with one choice has a unique preview; every
// choice of every slot (outfits, variants) is worn on the same skeleton.
std::vector<TagHash> read_equipment_entities(PackageManager& pm, const TagReader& r, size_t resource, bool all_choices) {
    const auto slots = r.vec_at(resource + 0x98);
    std::vector<TagHash> result;
    if (slots.element_class != 0x8080A320) return result;
    for (uint64_t i = 0; i < slots.count; ++i) {
        const auto choices = r.vec_at(slots.data + size_t(i) * 0x18 + 8);
        if (choices.element_class != 0x8080A322 || (!all_choices && choices.count != 1)) continue;
        for (uint64_t k = 0; k < choices.count; ++k) {
            const size_t row = choices.data + size_t(k) * 0x18;
            const auto child = resolve_wide_hash(pm, r.u32_at(row + 8), r.u64_at(row + 0x10));
            if (!child) continue;
            const Entry* entry = pm.entry(*child);
            if (entry && entry->type == 8 && entry->reference == kClassEntity) result.push_back(*child);
        }
    }
    return result;
}

std::vector<TagHash> read_motion_bank(PackageManager& pm, TagHash tag) {
    const Entry* entry = pm.entry(tag);
    if (!entry || entry->type != 8 || entry->reference != 0x80803BE9) return {};
    const auto bytes = pm.read(tag);
    TagReader r(bytes);
    const auto clips = r.vec_at(8);
    if (clips.element_class != 0x8080AE00 || clips.data > bytes.size() ||
        clips.count > (bytes.size() - clips.data) / 16) return {};
    std::vector<TagHash> result(size_t(clips.count));
    for (size_t i = 0; i < clips.count; ++i) {
        const size_t row = clips.data + 16 * i;
        const auto clip_tag = resolve_wide_hash(pm, r.u32_at(row), r.u64_at(row + 8));
        if (!clip_tag) continue;
        const auto* clip = pm.entry(*clip_tag);
        if (clip && clip->type == 8 && clip->reference == kClassAnimationClip) result[i] = *clip_tag;
    }
    return result;
}

struct ShellMotionBranch {
    std::set<uint32_t> shells;
    bool shared = false;
};

// S8080391D selects animation sequences (80803922, vectors of 80803924 clip index / weight).
// Its 80803968 decision rows encode the shell selector and the next row on a failed test.
// A negative signed result encodes the sequence index via one's complement.
std::map<size_t, ShellMotionBranch> read_shell_motion_branches(PackageManager& pm, TagHash graph,
                                                               const std::unordered_set<uint32_t>& shells) {
    const auto* entry = pm.entry(graph);
    if (!entry || entry->reference != 0x80803950) return {};
    const auto bytes = pm.read(graph);
    const TagReader r(bytes);
    std::map<size_t, std::set<uint32_t>> classified_leaves;
    for (size_t at = 4; at + 0x38 <= bytes.size(); at += 4) {
        if (r.u32_at(at - 4) != 0x8080391D) continue;
        try {
            const auto sequences = r.vec_at(at);
            const auto decisions = r.vec_at(at + 0x20);
            if (sequences.element_class != 0x80803922 || decisions.element_class != 0x80803968 ||
                sequences.data > bytes.size() || decisions.data > bytes.size() ||
                sequences.count > (bytes.size() - sequences.data) / 16 ||
                decisions.count > (bytes.size() - decisions.data) / 36) continue;
            std::map<size_t, uint32_t> selectors;
            for (size_t i = 0; i < decisions.count; ++i) {
                const size_t row = decisions.data + i * 36;
                const uint32_t hash = r.u32_at(row + 0x18);
                if (shells.contains(hash) && r.u32_at(row + 0x14) == 0x80000000 &&
                    r.u16_at(row + 0x20) < 0x8000) selectors.emplace(i, hash);
            }
            if (selectors.size() < 2) continue;
            for (const auto& [selector, shell] : selectors) {
                std::vector<size_t> pending{selector + 1};
                std::set<size_t> seen;
                while (!pending.empty()) {
                    const size_t i = pending.back();
                    pending.pop_back();
                    if (i >= decisions.count || selectors.contains(i) || !seen.insert(i).second) continue;
                    const size_t row = decisions.data + i * 36;
                    const auto next = r.u16_at(row + 0x1E);
                    if (next > i && next < decisions.count) pending.push_back(next);
                    const int result = int16_t(r.u16_at(row + 0x20));
                    if (result >= 0) {
                        pending.push_back(i + 1);
                        continue;
                    }
                    const size_t sequence = size_t(-result - 1);
                    if (sequence >= sequences.count) continue;
                    const auto leaves = r.vec_at(sequences.data + sequence * 16);
                    if (leaves.element_class != 0x80803924 || leaves.data > bytes.size() || leaves.count > (bytes.size() - leaves.data) / 8) continue;
                    for (size_t k = 0; k < leaves.count; ++k)
                        classified_leaves[leaves.data + 8 * k].insert(shell);
                }
            }
        } catch (const std::exception&) {}
    }
    std::map<size_t, ShellMotionBranch> result;
    if (classified_leaves.empty()) return result;
    for (size_t at = 8; at + 16 <= bytes.size(); at += 4) {
        // A leaf vector header is headed by the Tiger sentinel and followed by the typed rows.
        if (r.u32_at(at - 4) != 0x8080BFCD || r.u32_at(at + 8) != 0x80803924 || r.u32_at(at + 12)) continue;
        const auto count = r.u64_at(at);
        if (count > (bytes.size() - at - 16) / 8) continue;
        for (size_t i = 0; i < count; ++i) {
            const size_t leaf = at + 16 + i * 8;
            auto& entry = result[r.u32_at(leaf)];
            if (const auto found = classified_leaves.find(leaf); found != classified_leaves.end())
                entry.shells.insert(found->second.begin(), found->second.end());
            else entry.shared = true;
        }
    }
    return result;
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

bool same_skeleton(const Skeleton& a, const Skeleton& b) {
    return a.hashes == b.hashes && a.parents == b.parents && a.bind == b.bind && a.inverse_bind == b.inverse_bind;
}

bool same_rig(const AnimationRig& a, const AnimationRig& b) {
    return a.node_layout == b.node_layout && a.skeleton_bone == b.skeleton_bone &&
           a.default_pose == b.default_pose && a.bone_groups == b.bone_groups;
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
    rig.bone_groups = read_bone_groups(r, a + 0x98);
    if (nodes.data > r.size() || nodes.count > (r.size() - nodes.data) / 72)
        throw std::runtime_error("rig: node array out of bounds");
    const auto layout = r.span().subspan(nodes.data, size_t(nodes.count) * 72);
    rig.node_layout.assign(layout.begin(), layout.end());
    for (uint64_t i = 0; i < nodes.count; ++i) {
        rig.skeleton_bone.push_back(r.u16_at(to_skeleton.data + size_t(i) * 2));
        const int16_t space = int16_t(r.u16_at(nodes.data + size_t(i) * 72 + 8));
        rig.object_space.push_back(space == 2 || space == 6);
        const auto rest = read_transform(r, pose.data + size_t(i) * 32);
        const bool identity = std::fabs(rest.rotation[3]) > 0.999999f &&
                              std::ranges::all_of(rest.translation, [](float v) { return std::fabs(v) < 1e-5f; }) &&
                              std::fabs(rest.scale - 1) < 1e-5f;
        rig.bind_relative.push_back(space == 1 && identity);
        rig.default_pose.push_back(rest);
    }
    return rig;
}

ModelRigIndex index_model_skeletons(
    PackageManager& pm, const std::unordered_map<uint32_t, DynamicModelMaterials>& materials, ModelAssemblyIndex* assemblies, ModelMotionIndex* motions) {
    std::unordered_map<uint32_t, std::vector<uint32_t>> models_of;  // component -> models it places
    for (const auto& [model, m] : materials) models_of[m.component.value].push_back(model);
    std::unordered_set<uint32_t> skeletons, rigs;
    std::unordered_map<uint32_t, std::vector<TagHash>> children_of_component;
    std::unordered_map<uint32_t, std::vector<TagHash>> colocated_of_component;
    std::unordered_map<uint32_t, std::vector<TagHash>> equipment_of_component;
    std::unordered_map<uint32_t, std::pair<TagHash, TagHash>> bank_of_component;
    std::unordered_map<uint32_t, uint32_t> shell_of_component;
    std::unordered_set<uint32_t> shell_values;
    std::unordered_map<TagHash, std::vector<TagHash>> banks;
    std::unordered_map<TagHash, std::map<size_t, ShellMotionBranch>> shell_branches;
    if (motions) motions->clear();
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
                const auto [resource, cls] = resource_at(r, 0x18);
                if (cls == kClassSkeleton) skeletons.insert(tag.value);
                else if (cls == kClassAnimationRig) rigs.insert(tag.value);
                else if (cls == 0x808032B3) {
                    std::vector<TagHash> colocated;
                    children_of_component.emplace(tag.value, read_child_entities(pm, r, resource, assemblies ? &colocated : nullptr));
                    if (!colocated.empty()) colocated_of_component.emplace(tag.value, std::move(colocated));
                } else if (cls == 0x8080A313 && assemblies)
                    equipment_of_component.emplace(tag.value, read_equipment_entities(pm, r, resource, true));
                else if (motions && cls == 0x80803936)
                    bank_of_component.emplace(tag.value, std::pair{r.tag_at(resource + 0xE8), r.tag_at(resource + 0xF4)});
                else if (motions && cls == 0x808041DA && r.u32_at(resource + 0x140) == 0x808041DE) {
                    const uint32_t value = r.u32_at(resource + 0x148), shell = r.u32_at(resource + 0x14C);
                    if (value == 9 && shell && shell != 0x811C9DC5) {
                        shell_values.insert(shell);
                        shell_of_component.emplace(tag.value, shell);
                    }
                }
            } catch (...) {
            }
        }
    }
    struct EntityBinding {
        ModelRig binding;
        std::vector<uint32_t> models;
        std::vector<TagHash> children;
        std::vector<TagHash> colocated;
        std::vector<TagHash> equipment;
        std::vector<TagHash> clips;
    };
    struct MotionOwners {
        uint32_t shell = 0;
        bool conflicting = false;
        std::vector<TagHash> shell_clips;
    };
    std::unordered_map<uint32_t, MotionOwners> motion_owners;
    std::unordered_map<uint32_t, EntityBinding> bindings;
    std::unordered_map<uint32_t, ModelRig> out;
    std::unordered_map<TagHash, TagHash> rig_for_skeleton;
    std::unordered_set<TagHash> shared_skeletons;
    for (TagHash ent : entities) {
        try {
            auto bytes = pm.read(ent);
            TagReader r(bytes);
            auto comps = r.vec_at(0x08);
            EntityBinding node;
            uint32_t shell = 0;
            std::vector<TagHash> shell_clips;
            std::vector<std::pair<TagHash, TagHash>> motion_banks;
            for (uint64_t k = 0; k < comps.count; ++k) {
                const uint32_t c = r.u32_at(comps.data + size_t(k) * 12);
                if (skeletons.count(c)) node.binding.skeleton = TagHash(c);
                if (rigs.count(c)) node.binding.rig = TagHash(c);
                if (auto f = models_of.find(c); f != models_of.end())
                    node.models.insert(node.models.end(), f->second.begin(), f->second.end());
                if (auto f = children_of_component.find(c); f != children_of_component.end())
                    node.children.insert(node.children.end(), f->second.begin(), f->second.end());
                if (auto f = colocated_of_component.find(c); f != colocated_of_component.end())
                    node.colocated.insert(node.colocated.end(), f->second.begin(), f->second.end());
                if (auto f = equipment_of_component.find(c); f != equipment_of_component.end())
                    node.equipment.insert(node.equipment.end(), f->second.begin(), f->second.end());
                if (auto f = bank_of_component.find(c); f != bank_of_component.end() && !f->second.first.is_none())
                    motion_banks.push_back(f->second);
                if (auto f = shell_of_component.find(c); f != shell_of_component.end()) shell = f->second;
            }
            for (const auto& [bank_tag, graph_tag] : motion_banks) {
                const auto [bank, fresh] = banks.try_emplace(bank_tag);
                if (fresh) {
                    try { bank->second = read_motion_bank(pm, bank_tag); } catch (const std::exception&) {}
                }
                auto branch_index = shell_branches.end();
                if (shell && !graph_tag.is_none()) {
                    const auto [graph, fresh] = shell_branches.try_emplace(graph_tag);
                    if (fresh) {
                        try { graph->second = read_shell_motion_branches(pm, graph_tag, shell_values); }
                        catch (const std::exception&) {}
                    }
                    branch_index = graph;
                }
                for (size_t i = 0; i < bank->second.size(); ++i) {
                    if (bank->second[i].is_none()) continue;
                    if (branch_index != shell_branches.end()) {
                        const auto found = branch_index->second.find(i);
                        if (found != branch_index->second.end() && !found->second.shared && !found->second.shells.empty()) {
                            if (!found->second.shells.contains(shell)) continue;
                            shell_clips.push_back(bank->second[i]);
                        }
                    }
                    node.clips.push_back(bank->second[i]);
                }
            }
            if (motions && !node.clips.empty()) {
                std::ranges::sort(node.clips);
                node.clips.erase(std::unique(node.clips.begin(), node.clips.end()), node.clips.end());
                for (auto model : node.models) {
                    if (shell) {
                        auto& owner = motion_owners[model];
                        if (owner.shell && owner.shell != shell) owner.conflicting = true;
                        else owner.shell = shell;
                        owner.shell_clips.insert(owner.shell_clips.end(), shell_clips.begin(), shell_clips.end());
                    }
                    auto [owned, fresh] = motions->try_emplace(model, ModelMotionBank{node.clips, {}});
                    if (fresh || owned->second.clips == node.clips) continue;
                    // A mesh reused by several entities can show their common clips in its own
                    // preview. The individual owners' state machines may select different extras.
                    std::vector<TagHash> common;
                    std::ranges::set_intersection(owned->second.clips, node.clips, std::back_inserter(common));
                    owned->second.clips = std::move(common);
                }
            }
            if (!node.binding.skeleton.is_none()) {
                if (!node.binding.rig.is_none()) {
                    const auto [it, fresh] = rig_for_skeleton.emplace(node.binding.skeleton, node.binding.rig);
                    if (!fresh && it->second != node.binding.rig) shared_skeletons.insert(node.binding.skeleton);
                }
                for (uint32_t m : node.models) {
                    auto [it, fresh] = out.emplace(m, node.binding);
                    if (!fresh && it->second.rig.is_none()) it->second = node.binding;
                }
            }
            if (!node.models.empty() || !node.children.empty() || !node.equipment.empty() || !node.binding.skeleton.is_none())
                bindings.emplace(ent.value, std::move(node));
        } catch (...) {
        }
    }
    for (auto& [model, owner] : motion_owners) {
        if (owner.conflicting || owner.shell_clips.empty()) continue;
        auto& bank = motions->at(model);
        auto& own = owner.shell_clips;
        std::ranges::sort(own);
        own.erase(std::unique(own.begin(), own.end()), own.end());
        std::vector<TagHash> merged;
        std::ranges::set_union(bank.clips, own, std::back_inserter(merged));
        bank.clips = std::move(merged);
        bank.shell_clips = std::move(own);
    }
    std::unordered_map<uint32_t, ModelRig> inherited;
    std::unordered_set<uint32_t> ambiguous;
    for (const auto& [entity, node] : bindings) {
        if (node.binding.skeleton.is_none() || node.binding.rig.is_none() || node.children.empty()) continue;
        std::unordered_set<uint32_t> seen;
        std::vector<TagHash> pending = node.children;
        while (!pending.empty()) {
            const auto child = pending.back();
            pending.pop_back();
            if (!seen.insert(child.value).second) continue;
            const auto it = bindings.find(child.value);
            if (it == bindings.end() || !it->second.binding.skeleton.is_none() || !it->second.binding.rig.is_none()) continue;
            const auto& child_node = it->second;
            for (uint32_t model : child_node.models) {
                if (out.contains(model)) continue;
                const auto [found, fresh] = inherited.emplace(model, node.binding);
                if (!fresh && found->second != node.binding) ambiguous.insert(model);
            }
            pending.insert(pending.end(), child_node.children.begin(), child_node.children.end());
        }
    }
    for (const auto& [model, binding] : inherited)
        if (!ambiguous.contains(model)) out.emplace(model, binding);
    std::unordered_map<TagHash, Skeleton> known_skeletons;
    std::unordered_map<TagHash, TagHash> rig_for_copy;
    for (auto& [model, binding] : out) {
        if (!binding.rig.is_none() || shared_skeletons.contains(binding.skeleton)) continue;
        const auto it = rig_for_skeleton.find(binding.skeleton);
        if (it != rig_for_skeleton.end()) {
            binding.rig = it->second;
            continue;
        }
        if (const auto cached = rig_for_copy.find(binding.skeleton); cached != rig_for_copy.end()) {
            binding.rig = cached->second;
            continue;
        }
        TagHash selected;
        try {
            const auto target = load_skeleton(pm, binding.skeleton);
            for (const auto& [candidate, rig] : rig_for_skeleton) {
                if (shared_skeletons.contains(candidate)) continue;
                try {
                    auto cached = known_skeletons.find(candidate);
                    if (cached == known_skeletons.end())
                        cached = known_skeletons.emplace(candidate, load_skeleton(pm, candidate)).first;
                    if (!same_skeleton(target, cached->second)) continue;
                    if (!selected.is_none() && selected != rig &&
                        !same_rig(load_animation_rig(pm, selected), load_animation_rig(pm, rig))) {
                        selected = {};
                        break;
                    }
                    if (selected.is_none() || rig < selected) selected = rig;
                } catch (const std::exception&) {
                }
            }
        } catch (const std::exception&) {
        }
        binding.rig = selected;
        rig_for_copy.emplace(binding.skeleton, selected);
    }
    if (assemblies) {
        assemblies->clear();
        std::unordered_map<uint32_t, std::vector<TagHash>> by_root;
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> roots_of;
        std::unordered_set<uint32_t> ambiguous_roots;
        for (const auto& [entity, node] : bindings) {
            if (node.models.size() != 1 || (node.equipment.empty() && node.colocated.empty())) continue;
            const uint32_t root = node.models.front();
            std::vector<TagHash> pending = node.equipment;
            pending.insert(pending.end(), node.colocated.begin(), node.colocated.end());
            std::unordered_set<uint32_t> seen{entity};
            std::vector<TagHash> parts;
            while (!pending.empty()) {
                const auto child = pending.back();
                pending.pop_back();
                if (!seen.insert(child.value).second) continue;
                const auto it = bindings.find(child.value);
                if (it == bindings.end()) continue;
                for (uint32_t model : it->second.models) {
                    if (model == root) continue;
                    parts.emplace_back(model);
                }
                pending.insert(pending.end(), it->second.equipment.begin(), it->second.equipment.end());
                pending.insert(pending.end(), it->second.colocated.begin(), it->second.colocated.end());
            }
            std::ranges::sort(parts);
            parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
            const auto [found, fresh] = by_root.try_emplace(root, parts);
            if (!fresh && found->second != parts) ambiguous_roots.insert(root);
        }
        for (auto& [root, parts] : by_root) {
            if (parts.empty() || ambiguous_roots.contains(root)) continue;
            for (TagHash model : parts) roots_of[model.value].insert(root);
            parts.insert(parts.begin(), TagHash(root));
            assemblies->emplace(root, parts);
        }
        for (const auto& [model, roots] : roots_of) {
            if (roots.size() == 1) assemblies->try_emplace(model, assemblies->at(*roots.begin()));
        }
        // Models directly owned by the same entity share its object space. Keep rigid parts
        // too: machines and props can be split across several model components.
        std::unordered_map<uint32_t, uint32_t> bones_of;
        auto skinned_bones = [&](uint32_t model) -> uint32_t {
            if (const auto it = bones_of.find(model); it != bones_of.end()) return it->second;
            uint32_t bones = 0;
            try {
                const auto m = load_dynamic_model(pm, TagHash(model));
                if (std::ranges::any_of(m.meshes, [](const DynamicMesh& d) { return !d.skinning.is_none(); })) bones = std::max(1u, m.bone_count);
            } catch (const std::exception&) {
            }
            return bones_of[model] = bones;
        };
        std::unordered_map<uint32_t, std::vector<TagHash>> groups;
        std::unordered_set<uint32_t> grouped_twice;
        for (const auto& [entity, node] : bindings) {
            if (node.models.size() < 2) continue;
            std::vector<TagHash> parts;
            for (uint32_t model : node.models) parts.emplace_back(model);
            std::ranges::sort(parts);
            parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
            if (parts.size() < 2) continue;
            auto root = std::ranges::max_element(parts, [&](TagHash a, TagHash b) {
                const auto key = [&](TagHash t) {
                    const auto it = out.find(t.value);
                    return std::pair(it != out.end() && !it->second.rig.is_none(), skinned_bones(t.value));
                };
                return key(a) < key(b);
            });
            std::rotate(parts.begin(), root, root + 1);
            for (TagHash part : parts) {
                const auto [found, fresh] = groups.try_emplace(part.value, parts);
                if (!fresh && found->second != parts) grouped_twice.insert(part.value);
            }
        }
        for (const auto& [model, parts] : groups)
            if (!grouped_twice.contains(model)) assemblies->try_emplace(model, parts);
    }
    return out;
}

namespace {

int32_t matching_bind_joint(const Skeleton& source, const Skeleton& target, size_t bone) {
    const auto it = std::ranges::find(source.hashes, target.hashes[bone]);
    if (it == source.hashes.end()) return -1;
    if (std::find(it + 1, source.hashes.end(), *it) != source.hashes.end()) return -1;
    const auto index = size_t(it - source.hashes.begin());
    const auto& a = source.bind[index];
    const auto& b = target.bind[bone];
    float rotation_dot = 0;
    for (size_t i = 0; i < 4; ++i) rotation_dot += a.rotation[i] * b.rotation[i];
    if (std::fabs(rotation_dot) < 0.99999f || std::fabs(a.scale - b.scale) > 0.0001f) return -1;
    for (size_t i = 0; i < 3; ++i)
        if (std::fabs(a.translation[i] - b.translation[i]) > 0.0001f) return -1;
    return int32_t(index);
}

} // namespace

std::vector<int32_t> assembly_pose_map(const Skeleton& source, const Skeleton& target) {
    std::vector<int32_t> direct;
    for (size_t b = 0; b < target.size(); ++b) direct.push_back(matching_bind_joint(source, target, b));
    std::vector<int32_t> result = direct;
    for (size_t b = 0; b < target.size(); ++b) {
        int32_t p = int32_t(b);
        for (size_t depth = 0; p >= 0 && depth < target.size(); ++depth) {
            if (direct[size_t(p)] >= 0) {
                result[b] = direct[size_t(p)];
                break;
            }
            p = target.parents[size_t(p)];
        }
    }
    return result;
}

std::optional<ModelRig> find_assembly_rig(PackageManager& pm, const ModelRigIndex& models, uint32_t bones,
                                           const Skeleton& other) {
    if (!bones) return std::nullopt;
    std::optional<ModelRig> result;
    Skeleton matched_skeleton;
    AnimationRig matched_rig;
    std::set<std::pair<TagHash, TagHash>> seen;
    for (const auto& [model, binding] : models) {
        if (binding.rig.is_none() || !seen.emplace(binding.skeleton, binding.rig).second) continue;
        try {
            auto skeleton = load_skeleton(pm, binding.skeleton);
            if (skeleton.size() != bones) continue;
            size_t shared = 0;
            for (size_t b = 0; b < other.size(); ++b)
                if (other.parents[b] >= 0 && matching_bind_joint(skeleton, other, b) >= 0) ++shared;
            if (shared < 3) continue;
            auto rig = load_animation_rig(pm, binding.rig);
            if (!result) {
                result = binding;
                matched_skeleton = std::move(skeleton);
                matched_rig = std::move(rig);
            } else if (!same_skeleton(matched_skeleton, skeleton) || !same_rig(matched_rig, rig)) {
                return std::nullopt;
            } else if (std::pair(binding.skeleton, binding.rig) < std::pair(result->skeleton, result->rig)) result = binding;
        } catch (const std::exception&) {
        }
    }
    return result;
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
    clip.bone_groups = read_bone_groups(r, 0x170);
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
                TagReader r(bytes);
                out.push_back({tag, r.u16_at(0x140), r.u16_at(0x142), read_bone_groups(r, 0x170), r.u32_at(0x120)});
            } catch (...) {
            }
        }
    }
    return out;
}

std::vector<ClipInfo> compatible_animation_clips(PackageManager& pm, const ModelRigIndex& models,
                                                const AnimationRig& rig, std::span<const ClipInfo> clips) {
    std::vector<ClipInfo> result;
    for (const auto& clip : clips)
        if (clip.bones == rig.size() && !clip.bone_groups.empty() && clip.bone_groups == rig.bone_groups)
            result.push_back(clip);
    if (!result.empty() || rig.node_layout.empty() ||
        std::none_of(clips.begin(), clips.end(), [&](const ClipInfo& clip) {
            return clip.bones == rig.size() && !clip.bone_groups.empty();
        })) return result;

    std::unordered_set<TagHash> seen{rig.component};
    std::vector<std::vector<AnimationBoneGroup>> equivalent_groups;
    for (const auto& [model, binding] : models) {
        if (binding.rig.is_none() || !seen.insert(binding.rig).second) continue;
        try {
            const auto candidate = load_animation_rig(pm, binding.rig);
            if (!candidate.bone_groups.empty() && candidate.node_layout == rig.node_layout &&
                candidate.skeleton_bone == rig.skeleton_bone && candidate.default_pose == rig.default_pose)
                equivalent_groups.push_back(candidate.bone_groups);
        } catch (const std::exception&) {
        }
    }
    for (const auto& clip : clips)
        if (clip.bones == rig.size() && std::ranges::find(equivalent_groups, clip.bone_groups) != equivalent_groups.end())
            result.push_back(clip);
    return result;
}

std::optional<SkinFit> fit_skin_to_skeleton(PackageManager& pm, const ModelRigIndex& models, uint32_t bones,
                                            std::span<const std::array<float, 3>> positions,
                                            std::span<const std::array<uint16_t, 4>> vertex_bones,
                                            std::span<const std::array<float, 4>> vertex_weights,
                                            std::span<const ClipInfo> clips, std::vector<SkinFit>* ranking) {
    if (!bones || positions.empty() || vertex_bones.size() != positions.size() || vertex_weights.size() != positions.size())
        return std::nullopt;
    // Up to ~4000 vertices, each with its dominant bone.
    std::vector<std::pair<size_t, uint16_t>> samples;
    const size_t step = std::max<size_t>(1, positions.size() / 4000);
    for (size_t v = 0; v < positions.size(); v += step) {
        size_t k = 0;
        for (size_t j = 1; j < 4; ++j)
            if (vertex_weights[v][j] > vertex_weights[v][k]) k = j;
        if (vertex_weights[v][k] < 0.5f || vertex_bones[v][k] >= bones) continue;
        samples.emplace_back(v, vertex_bones[v][k]);
    }
    if (samples.size() < 16) return std::nullopt;
    struct Candidate {
        ModelRig binding;
        float error = 0;
        Skeleton skeleton;
        size_t clips = 0;
    };
    std::vector<Candidate> candidates;
    std::set<std::pair<TagHash, TagHash>> seen;
    for (const auto& [model, binding] : models) {
        if (binding.rig.is_none() || !seen.emplace(binding.skeleton, binding.rig).second) continue;
        try {
            Skeleton sk = load_skeleton(pm, binding.skeleton);
            if (sk.size() != bones) continue;
            // Each bone's segments: to its children, or a point without children.
            std::vector<std::vector<size_t>> children(sk.size());
            for (size_t b = 0; b < sk.size(); ++b)
                if (sk.parents[b] >= 0) children[size_t(sk.parents[b])].push_back(b);
            double sum = 0;
            for (const auto& [v, b] : samples) {
                const auto& p = positions[v];
                const auto& a = sk.bind[b].translation;
                auto dist = [&](const std::array<float, 3>& e) {
                    const float d[3]{e[0] - a[0], e[1] - a[1], e[2] - a[2]};
                    const float w[3]{p[0] - a[0], p[1] - a[1], p[2] - a[2]};
                    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    const float t = dd > 1e-12f ? std::clamp((w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / dd, 0.0f, 1.0f) : 0.0f;
                    const float x = w[0] - d[0] * t, y = w[1] - d[1] * t, z = w[2] - d[2] * t;
                    return std::sqrt(x * x + y * y + z * z);
                };
                float best = dist(a);
                for (size_t c : children[b]) best = std::min(best, dist(sk.bind[c].translation));
                sum += best;
            }
            Candidate c{binding, float(sum / double(samples.size())), std::move(sk), 0};
            if (!clips.empty()) {
                try {
                    c.clips = compatible_animation_clips(pm, models, load_animation_rig(pm, binding.rig), clips).size();
                } catch (const std::exception&) {
                }
            }
            candidates.push_back(std::move(c));
        } catch (const std::exception&) {
        }
    }
    if (candidates.empty()) return std::nullopt;
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.error != b.error ? a.error < b.error : a.binding.rig < b.binding.rig;
    });
    if (ranking)
        for (const auto& c : candidates) ranking->push_back({c.binding, c.error});
    // Skin vertices lie on the surface, a few centimetres from the bone segments. Skeletons that fit
    // to within 1 mm are the same bind pose under another tag: among those take the rig with the
    // most clips. Another skeleton must fit clearly worse (1.5x), else the choice is ambiguous.
    const float best_error = candidates.front().error;
    if (best_error > 0.2f) return std::nullopt;
    const Candidate* best = &candidates.front();
    for (const auto& c : candidates) {
        if (c.error > best_error + 0.001f) {
            if (c.error < best_error * 1.5f) return std::nullopt;
            break;
        }
        if (c.clips > best->clips) best = &c;
    }
    return SkinFit{best->binding, best->error};
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
    // Each tracked bone whose default pose is clearly away from identity votes for the pose its
    // first frame is nearer to: a delta stays near identity (zero offset), a full pose near the
    // default. Partial deltas (faces, upper body) move several bones by 10-20 degrees, so the
    // nearer pose decides rather than a fixed identity threshold.
    size_t votes = 0, at_identity = 0;
    auto angle = [](const Q& a, const Q& b) {
        return std::acos(std::min(1.0f, std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3])));
    };
    for (const auto& t : clip.rotations) {
        if (t.bone >= rig.size() || t.frames.empty()) continue;
        const Q& rest = rig.default_pose[t.bone].rotation;
        const float to_rest = angle(t.frames[0], rest), to_identity = angle(t.frames[0], {0, 0, 0, 1});
        if (angle(rest, {0, 0, 0, 1}) < 0.25f) continue;  // under ~30 degrees: identity in a full pose too
        ++votes;
        if (to_identity < to_rest) ++at_identity;
    }
    for (const auto& t : clip.translations) {
        if (t.bone >= rig.size() || t.frames.empty() || rig.object_space[t.bone]) continue;
        const auto& rest = rig.default_pose[t.bone].translation;
        const float rest_len = std::sqrt(rest[0] * rest[0] + rest[1] * rest[1] + rest[2] * rest[2]);
        if (rest_len < 0.01f) continue;
        const auto& v = t.frames[0];
        const float to_zero = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const float to_rest = std::sqrt((v[0] - rest[0]) * (v[0] - rest[0]) + (v[1] - rest[1]) * (v[1] - rest[1]) +
                                        (v[2] - rest[2]) * (v[2] - rest[2]));
        ++votes;
        if (to_zero < to_rest) ++at_identity;
    }
    return votes > 0 && at_identity * 2 > votes;
}

void pose_skeleton(const Skeleton& sk, const AnimationRig& rig, const AnimationClip& clip, float frame,
                   std::vector<BoneTransform>& object, bool authored_local_offsets) {
    std::vector<BoneTransform> pose = rig.default_pose;
    // Clips shared by rigs of other proportions carry their own limb offsets. A parent-relative
    // translation track that never comes near this rig's rest offset was authored for another
    // body: keep the bind offset so limbs keep their length. Tracks that start from the rest
    // offset (sliding parts, jaws) play as authored.
    std::vector<uint8_t> foreign_offset(rig.size(), 0);
    // Weapon and mechanism rigs can mix full local transforms and bind-relative offsets.
    // Their largely parent-relative layouts keep the existing track-based interpretation.
    const bool mechanism_offsets = std::ranges::count(rig.object_space, uint8_t(1)) <= 3;
    const auto identity_local = [&](size_t bone) { return bone < rig.bind_relative.size() && rig.bind_relative[bone]; };
    std::vector<int32_t> rig_to_skeleton_parent_child(rig.size(), -1);
    for (size_t i = 0; i < rig.size(); ++i)
        if (rig.skeleton_bone[i] < sk.size()) rig_to_skeleton_parent_child[i] = rig.skeleton_bone[i];
    std::vector<uint8_t> offset_translation(rig.size(), 0), offset_rotation(rig.size(), 1);
    const bool additive = is_additive(rig, clip);
    if (!additive) {
        for (const auto& t : clip.rotations) {
            if (!mechanism_offsets || t.bone >= rig.size() || !identity_local(t.bone) || t.frames.empty() || rig_to_skeleton_parent_child[t.bone] < 0) continue;
            const Q& bind = sk.local_bind[size_t(rig_to_skeleton_parent_child[t.bone])].rotation;
            float to_bind = 1e30f, to_identity = 1e30f;
            for (const auto& q : t.frames) {
                to_bind = std::min(to_bind, 1 - std::fabs(q[0] * bind[0] + q[1] * bind[1] + q[2] * bind[2] + q[3] * bind[3]));
                to_identity = std::min(to_identity, 1 - std::fabs(q[3]));
            }
            offset_rotation[t.bone] = to_identity <= to_bind;
        }
        for (const auto& t : clip.translations) {
            if (!mechanism_offsets || t.bone >= rig.size() || !identity_local(t.bone) || t.frames.empty() || rig_to_skeleton_parent_child[t.bone] < 0) continue;
            const auto& bind = sk.local_bind[size_t(rig_to_skeleton_parent_child[t.bone])].translation;
            const float length = std::sqrt(bind[0] * bind[0] + bind[1] * bind[1] + bind[2] * bind[2]);
            const float tolerance = std::max(0.005f, 0.1f * length);
            float to_bind = 1e30f, to_zero = 1e30f;
            for (const auto& v : t.frames) {
                const float x = v[0] - bind[0], y = v[1] - bind[1], z = v[2] - bind[2];
                to_bind = std::min(to_bind, std::sqrt(x * x + y * y + z * z));
                to_zero = std::min(to_zero, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            }
            // 1: an offset from the bind pose; 2: the full offset; 0: another body's, keep the bind.
            // A full transform must reproduce this skeleton's own offset; a track only near it was authored
            // for another body sharing the rig (its proportions) and keeps the bind.
            offset_translation[t.bone] = to_zero <= tolerance && to_zero <= to_bind ? 1 : to_bind <= std::max(0.005f, 0.01f * length) ? 2 : 0;
        }
        for (const auto& t : clip.translations) {
            if (t.bone >= rig.size() || rig.object_space[t.bone] || t.frames.empty()) continue;
            const auto& rest = rig.default_pose[t.bone].translation;
            const float length = std::sqrt(rest[0] * rest[0] + rest[1] * rest[1] + rest[2] * rest[2]);
            const float tolerance = std::max(0.005f, 0.1f * length);
            float nearest = 1e30f;
            for (const auto& v : t.frames) {
                const float x = v[0] - rest[0], y = v[1] - rest[1], z = v[2] - rest[2];
                nearest = std::min(nearest, std::sqrt(x * x + y * y + z * z));
            }
            if (!authored_local_offsets && nearest > tolerance) foreign_offset[t.bone] = 1;
        }
    }
    if (additive) {
        std::vector<BoneTransform> delta(rig.size());
        sample_clip(clip, frame, delta);
        for (size_t i = 0; i < rig.size(); ++i) {
            pose[i].rotation = qnormalize(qmul(pose[i].rotation, delta[i].rotation));
            for (int k = 0; k < 3; ++k) pose[i].translation[k] += delta[i].translation[k];
            pose[i].scale *= delta[i].scale;
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
    std::vector<uint8_t> below_wrist(n, 0);
    // Authored skeleton bone hashes shared by the humanoid wrists.
    constexpr uint32_t wrist_a = 0x67CB838C, wrist_b = 0xEA6B9DDE;
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
                if (ri >= 0) o.rotation = qnormalize(pose[size_t(ri)].rotation);
            } else {
                const uint32_t parent_hash = size_t(p) < sk.hashes.size() ? sk.hashes[size_t(p)] : 0;
                below_wrist[b] = below_wrist[size_t(p)] || parent_hash == wrist_a || parent_hash == wrist_b;
                const BoneTransform& po = object[size_t(p)];
                auto local = sk.local_bind[b];
                if (ri >= 0 && !rig.object_space[size_t(ri)]) {
                    const auto& c = pose[size_t(ri)];
                    const auto& rest = rig.default_pose[size_t(ri)];
                    const bool bind_digit = below_wrist[b] && identity_local(size_t(ri));
                    if (mechanism_offsets && identity_local(size_t(ri)) && !bind_digit && !additive) {
                        // Local-mode weapon rigs can store offsets from the model bind pose.
                        local.rotation = offset_rotation[size_t(ri)] ? qnormalize(qmul(c.rotation, local.rotation)) : c.rotation;
                        if (offset_translation[size_t(ri)] == 1)
                            for (int k = 0; k < 3; ++k) local.translation[k] += c.translation[k];
                        else if (offset_translation[size_t(ri)] == 2)
                            local.translation = c.translation;
                        if (c.scale > 0 && std::isfinite(c.scale)) local.scale *= c.scale;
                    } else {
                        // The identity-rest digit channels bend about the finger's own bind axes.
                        // An identity key preserves the joint's bind orientation, including thumb opposition.
                        local.rotation = bind_digit ? qnormalize(qmul(local.rotation, c.rotation)) : qnormalize(c.rotation);
                        // A matching rest offset establishes that this track uses the local
                        // skeleton space and units. Variant rigs can have different proportions.
                        float rest_error = 0;
                        for (int k = 0; k < 3; ++k)
                            rest_error = std::max(rest_error, std::fabs(rest.translation[k] - local.translation[k]));
                        if (rest_error <= 0.0001f && !foreign_offset[size_t(ri)]) local.translation = c.translation;
                        if (std::fabs(local.scale - rest.scale) <= 0.001f && c.scale > 0 && std::isfinite(c.scale))
                            local.scale = c.scale;
                    }
                }
                o = po * local;
                if (ri >= 0 && rig.object_space[size_t(ri)]) {
                    const auto& c = pose[size_t(ri)];
                    o.rotation = qnormalize(c.rotation);
                    if (sk.parents[size_t(p)] < 0)
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
