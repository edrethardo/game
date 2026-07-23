// class_defs.cpp — the player class table (kClassDefs), moved out of engine_init.cpp so
// tests-only code (the balance lab) can link the real class base stats and skill lists
// without dragging in the whole Engine. Declared extern in game/item.h.
#include "game/item.h"

// ---------------------------------------------------------------------------
// Player class definitions — 8 classes with 4 skills each
// ---------------------------------------------------------------------------
const ClassDef kClassDefs[static_cast<u32>(PlayerClass::CLASS_COUNT)] = {
    // WARRIOR — Melee Tank
    {"Warrior", "Heavy melee fighter with crowd control",
     150.0f, 5.0f, 80.0f, "Iron Sword",
     {SkillId::THUNDERCLAP, SkillId::WAR_CRY, SkillId::WHIRLWIND, SkillId::EARTHQUAKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE,
     "player_warrior", "player_warrior_skin"},

    // RANGER — Rapid-Fire Sharpshooter
    {"Ranger", "Rapid-fire sharpshooter with piercing shots and volleys",
     80.0f, 6.5f, 100.0f, "Short Bow",
     {SkillId::VOLLEY, SkillId::PIERCING_SHOT, SkillId::BARRAGE, SkillId::MARK_PREY},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::PROJECTILE,
     "player_ranger", "player_ranger_skin"},

    // SORCERER — Glass Cannon
    {"Sorcerer", "Devastating elemental magic, fragile body",
     70.0f, 5.5f, 150.0f, "Wand of Sparks",
     {SkillId::FIREBALL, SkillId::FROZEN_ORB, SkillId::CHAIN_LIGHTNING, SkillId::METEOR_STRIKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::PROJECTILE,
     "player_sorcerer", "player_sorcerer_skin"},

    // ROGUE — Hit-and-Run Assassin
    {"Rogue", "Hit-and-run assassin with stealth and backstabs",
     85.0f, 7.0f, 100.0f, "Rusty Dagger",
     {SkillId::FAN_OF_KNIVES, SkillId::SHADOW_STEP, SkillId::POISON_CLOUD, SkillId::SHADOW_DANCE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE,
     "player_rogue", "player_rogue_skin"},

    // PALADIN — Wrathful Holy Warrior
    {"Paladin", "Wrathful holy warrior raining judgment",
     130.0f, 5.0f, 90.0f, "Iron Sword",
     {SkillId::HOLY_SMITE, SkillId::HOLY_BOMBARDMENT, SkillId::HOLY_NOVA, SkillId::DIVINE_JUDGMENT},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE,
     "player_paladin", "player_paladin_skin"},

    // COMBAT ENGINEER — Gadget Specialist
    {"Combat Engineer", "Turrets, tesla coils, and gadgets",
     100.0f, 5.5f, 120.0f, "Pistol",
     {SkillId::SHOCK_BOLT, SkillId::DEPLOY_TURRET, SkillId::TESLA_COIL, SkillId::MECH_OVERDRIVE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN,
     "player_combat_engineer", "player_combat_engineer_skin"},

    // MARKSMAN — Precision Sniper
    {"Marksman", "Anti-materiel sniper with devastating penetrating shots",
     75.0f, 6.0f, 100.0f, "Revolver",
     {SkillId::AIMED_SHOT, SkillId::EXPLOSIVE_ROUND, SkillId::OVERCHARGED_MAGAZINE, SkillId::HEADSHOT},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN,
     "player_marksman", "player_marksman_skin"},

    // TINKERER — Swarm Overlord
    {"Tinkerer", "Swarm overlord who overwhelms with drone armies",
     90.0f, 5.5f, 110.0f, "Pistol",
     {SkillId::SWARM_DEPLOY, SkillId::OVERCLOCK, SkillId::DETONATE_SWARM, SkillId::SWARM_QUEEN},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN,
     "player_tinkerer", "player_tinkerer_skin"},

    // WANDERER — evasive counter-attacker, dodge roll replaces block
    {
        "Wanderer",
        "Evasive counter-attacker. Dodge through enemy attacks for counter-hits and stacking attack speed.",
        90.0f,   // baseHealth — low, dodge is the defense
        6.5f,    // baseMoveSpeed — fast, tied with Ranger
        110.0f,  // baseEnergy
        "Iron Sword",
        {SkillId::DEFLECT, SkillId::EXPLOIT_WEAKNESS, SkillId::ADRENALINE_SURGE, SkillId::DEATHS_DANCE},
        {1, 10, 20, 30},   // skill unlock floors
        {5, 20, 30, 40},   // skill upgrade floors
        WeaponType::MELEE,  // +20% melee damage
        "player_wanderer", "player_wanderer_skin"
    },
};
