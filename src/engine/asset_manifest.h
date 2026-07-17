#pragma once

#include "core/types.h"

// asset_manifest.h — the single source of truth for every mesh the engine loads by name.
//
// WHY THIS IS A SHARED HEADER AND NOT AN INLINE TABLE IN THE LOADER:
//
// `assets/meshes/*.obj` is GITIGNORED. Meshes are BUILT by `tools/build_assets.py`, never
// committed. So a mesh that this table names but the generator does not produce simply DOES NOT
// EXIST on CI or in any release build — ObjLoader logs one WARN into a boot log full of INFO,
// the loader skips the entry, and the game ships that prop with no model at all. It works
// perfectly on the machine that generated the .obj by hand and is invisible everywhere else.
// That is exactly how the loot goblin and the shrine came within one commit of shipping with no
// mesh whatsoever.
//
// Hoisting the table here makes the manifest DATA that two independent checks can read:
//   1. `tools/build_assets.py` parses this file and hard-fails if it does not produce every mesh
//      named below. That check runs in CI's "Generate assets" step on every platform, which is
//      the exact place the bug bites — and it compares against the files THIS RUN wrote, so a
//      stale .obj lying around in a dev's working tree cannot mask a missing generator entry.
//   2. `tests/engine/test_asset_manifest.cpp` pins the C++-side invariants (no duplicate names,
//      fits the registry, well-formed paths).
//
// Add a mesh here AND to tools/build_assets.py. Either one alone now fails loudly.

// Registry capacity. `Engine::m_meshDefs` is sized by this and the load loop BREAKS when it is
// full — an overflow silently drops the TAIL of the table (the player meshes), so the
// static_assert below is the only thing standing between "added a mesh" and "the wanderer has no
// model". Bump this, don't trim the table.
constexpr u32 MESH_DEF_CAPACITY = 112;

struct MeshAsset { const char* name; const char* path; };

// Slot 0 of the registry is the built-in cube fallback, so the usable capacity is one less.
static constexpr MeshAsset kMeshAssets[] = {
    {"skeleton",       "assets/meshes/skeleton.obj"},
    {"spider",         "assets/meshes/spider.obj"},
    {"bat",            "assets/meshes/bat.obj"},
    {"pillar",         "assets/meshes/pillar.obj"},
    {"chest",          "assets/meshes/chest.obj"},
    {"goblin",         "assets/meshes/goblin.obj"},   // loot goblin (floor event)
    {"shrine",         "assets/meshes/shrine.obj"},   // walk-up buff shrine
    {"sword",          "assets/meshes/sword.obj"},
    {"dagger",         "assets/meshes/dagger.obj"},
    {"axe",            "assets/meshes/axe.obj"},
    {"pistol",         "assets/meshes/pistol.obj"},
    {"smg",            "assets/meshes/smg.obj"},
    {"carbine",        "assets/meshes/carbine.obj"},
    {"revolver",       "assets/meshes/revolver.obj"},
    {"bow",            "assets/meshes/bow.obj"},
    {"crossbow",       "assets/meshes/crossbow.obj"},
    {"throwing_knife", "assets/meshes/throwing_knife.obj"},
    {"molotov",        "assets/meshes/molotov.obj"},
    {"chakram",        "assets/meshes/chakram.obj"},
    {"infinity_chakram", "assets/meshes/infinity_chakram.obj"},
    {"gloves",         "assets/meshes/gloves.obj"},
    {"helmet",         "assets/meshes/helmet.obj"},
    {"armor",          "assets/meshes/armor.obj"},
    {"boots",          "assets/meshes/boots.obj"},
    // Armor tier variants (MEDIUM reuses the bare meshes above; chest MEDIUM = "armor").
    // Resolved per armor ItemDef into ItemDef.tierMeshId; rendered on the body / inspect screen.
    {"helmet_light",   "assets/meshes/helmet_light.obj"},
    {"helmet_heavy",   "assets/meshes/helmet_heavy.obj"},
    {"chest_light",    "assets/meshes/chest_light.obj"},
    {"chest_heavy",    "assets/meshes/chest_heavy.obj"},
    {"boots_light",    "assets/meshes/boots_light.obj"},
    {"boots_heavy",    "assets/meshes/boots_heavy.obj"},
    {"steadfast_greaves", "assets/meshes/steadfast_greaves.obj"},  // legendary anti-CC boots
    {"gloves_light",   "assets/meshes/gloves_light.obj"},
    {"gloves_heavy",   "assets/meshes/gloves_heavy.obj"},
    {"ring",           "assets/meshes/ring.obj"},
    {"shield",         "assets/meshes/shield.obj"},
    {"human",          "assets/meshes/human.obj"},
    {"wand",           "assets/meshes/wand.obj"},
    {"mace",           "assets/meshes/mace.obj"},
    {"cleric",         "assets/meshes/cleric.obj"},
    {"archer",         "assets/meshes/archer.obj"},
    {"mage",           "assets/meshes/mage.obj"},
    {"rogue",          "assets/meshes/rogue.obj"},
    {"paladin",        "assets/meshes/paladin.obj"},
    {"staff",          "assets/meshes/staff.obj"},
    {"web",            "assets/meshes/web.obj"},
    {"web_wall",       "assets/meshes/web_wall.obj"},
    {"shackles",       "assets/meshes/shackles.obj"},
    {"barrel",         "assets/meshes/barrel.obj"},
    {"cage",           "assets/meshes/cage.obj"},
    {"bones",          "assets/meshes/bones.obj"},
    {"brazier",        "assets/meshes/brazier.obj"},
    {"butcher",        "assets/meshes/butcher.obj"},
    {"cleaver",        "assets/meshes/cleaver.obj"},
    {"iron_maiden",    "assets/meshes/iron_maiden.obj"},
    {"arrow",          "assets/meshes/arrow.obj"},
    {"bolt",           "assets/meshes/bolt.obj"},
    {"skeleton_arm",   "assets/meshes/skeleton_arm.obj"},
    {"skeleton_leg",   "assets/meshes/skeleton_leg.obj"},
    {"bat_wing_mesh",  "assets/meshes/bat_wing_mesh.obj"},
    {"butcher_arm",    "assets/meshes/butcher_arm.obj"},
    {"butcher_leg",    "assets/meshes/butcher_leg.obj"},
    {"bat_foot",       "assets/meshes/bat_foot.obj"},
    {"andariel",       "assets/meshes/andariel.obj"},
    {"spider_leg_pair","assets/meshes/spider_leg_pair.obj"},
    {"claymore",       "assets/meshes/claymore.obj"},
    {"turret",         "assets/meshes/turret.obj"},
    {"gargoyle",       "assets/meshes/gargoyle.obj"},
    {"necromancer",    "assets/meshes/necromancer.obj"},
    {"shaman",         "assets/meshes/shaman.obj"},
    {"herald",         "assets/meshes/herald.obj"},
    // New enemy meshes (roster rework)
    {"hellhound",      "assets/meshes/hellhound.obj"},
    {"wraith",         "assets/meshes/wraith.obj"},
    {"sentinel",       "assets/meshes/sentinel.obj"},
    {"cave_troll",     "assets/meshes/cave_troll.obj"},
    {"pit_fiend",      "assets/meshes/pit_fiend.obj"},
    {"pit_fiend_wing","assets/meshes/pit_fiend_wing.obj"},
    {"hellforge_smith","assets/meshes/hellforge_smith.obj"},
    {"succubus",       "assets/meshes/succubus.obj"},
    {"abyssal_titan",  "assets/meshes/abyssal_titan.obj"},
    {"entropy_weaver", "assets/meshes/entropy_weaver.obj"},
    // New boss meshes (visual rework) — each major boss now has a dedicated OBJ
    {"lich",           "assets/meshes/lich.obj"},
    {"warden",         "assets/meshes/warden.obj"},
    {"spider_queen",   "assets/meshes/spider_queen.obj"},
    {"korvath",        "assets/meshes/korvath.obj"},
    {"azhar",          "assets/meshes/azhar.obj"},
    {"diabro",         "assets/meshes/diabro.obj"},
    {"nyx",            "assets/meshes/nyx.obj"},
    {"reaper",         "assets/meshes/reaper.obj"},
    // Secret superboss: The Dungeon Engine + its source-shard pickup key.
    {"engine",         "assets/meshes/engine.obj"},
    {"shard",          "assets/meshes/shard.obj"},
    // Player class meshes — resolved by name from ClassDef.meshName in the renderer.
    // Distinct from the NPC meshes above so player and town-NPC visuals can diverge.
    // The renderer falls back to "human" if any of these fails to load (e.g. the
    // generator hasn't been run yet), so this is safe to register pre-asset-build.
    {"player_warrior",         "assets/meshes/player_warrior.obj"},
    {"player_ranger",          "assets/meshes/player_ranger.obj"},
    {"player_sorcerer",        "assets/meshes/player_sorcerer.obj"},
    {"player_rogue",           "assets/meshes/player_rogue.obj"},
    {"player_paladin",         "assets/meshes/player_paladin.obj"},
    {"player_combat_engineer", "assets/meshes/player_combat_engineer.obj"},
    {"player_marksman",        "assets/meshes/player_marksman.obj"},
    {"player_tinkerer",        "assets/meshes/player_tinkerer.obj"},
    {"player_wanderer",        "assets/meshes/player_wanderer.obj"},
};
constexpr u32 kMeshAssetCount = sizeof(kMeshAssets) / sizeof(kMeshAssets[0]);

// The registry has a SECOND producer: after this table loads, LimbSystem::init appends its
// procedural box limbs (arm/leg/spider-leg/mandible/wing/claw) into the same array. They must be
// budgeted here or they overflow — and an overflowed limb resolves to mesh 0, the fallback CUBE.
// That is not hypothetical: the limb registration carried a hardcoded cap of 64 while this registry
// grew to 112, so all six limbs silently became cubes and every spider wore two of them as
// mandibles. engine_init_assets.cpp static_asserts that this number still equals
// LimbSystem::LIMB_MESH_COUNT, so the two cannot drift.
constexpr u32 kLimbMeshReserve = 6;

static_assert(kMeshAssetCount + 1 + kLimbMeshReserve <= MESH_DEF_CAPACITY,
              "mesh registry too small — the loader would silently drop the tail of this list, and "
              "a dropped mesh renders as the fallback CUBE. Budget = this table + 1 (the cube in "
              "slot 0) + LimbSystem's box limbs. Raise MESH_DEF_CAPACITY.");

// Decoration props: loaded for their CPU geometry only and BAKED into floor sections by the level
// mesher, so they cost no draw calls and have no collision. Same gitignore trap applies.
struct PropAsset { const char* path; const char* material; f32 radius; };

static constexpr PropAsset kPropAssets[] = {
    {"assets/meshes/rubble.obj",   "prop_iron", 0.30f},
    {"assets/meshes/rock.obj",     "prop_iron", 0.30f},
    {"assets/meshes/bones.obj",    "prop_bone", 0.30f},
    {"assets/meshes/mushroom.obj", "prop_wood", 0.20f},
    {"assets/meshes/crackbit.obj", "prop_iron", 0.30f},
};
constexpr u32 kPropAssetCount = sizeof(kPropAssets) / sizeof(kPropAssets[0]);
