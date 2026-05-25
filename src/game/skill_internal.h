#pragma once
// skill_internal.h — linkage-only header for the skill fire* helper functions.
// NOT part of the public API. Included by skill_system.cpp (for tryActivate dispatch)
// and by the per-class family .cpp files that define each fire* function.
//
// Rules:
//   - All fire* functions are declared here as plain free functions (no static).
//   - Cross-boundary module-scope state is declared extern here and defined once
//     in skill_system.cpp.
//   - Do NOT include this header from any file outside src/game/skill_*.cpp.

#include "game/skill.h"
#include "game/player.h"
#include "game/combat.h"
#include "audio/audio.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "renderer/debug_draw.h"
#include "renderer/particles.h"
#include "renderer/camera.h"
#include "core/log.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Cross-boundary state — defined once in skill_system.cpp, extern everywhere else
// ---------------------------------------------------------------------------

// The pending meteor/pillar pool — also extern'd by engine.cpp for render circles
extern PendingMeteor s_meteors[MAX_PENDING_METEORS];

// Skill power scaling (0.0 = base, 1.0 = max). Set by engine before tryActivate.
extern f32 s_skillPower;

// Class skill damage multiplier — scales all class skill damage/heal numbers.
extern f32 s_classDmgMult;

// Equipped weapon base damage — Marksman/Ranger skills scale off this.
extern f32 s_weaponDamage;

// Arrow mesh ID for Volley projectiles.
extern u8 s_arrowMeshId;

// Bolt mesh/material IDs for Shock Bolt projectiles.
extern u8 s_boltMeshId;
extern u8 s_shockBoltMatId;

// Particle pool and screen shake — wired by Engine::init via setFXTargets.
extern ParticlePool* s_particlePool;
extern ScreenShake*  s_screenShake;

// Visual FX callbacks — set by Engine via SkillSystem::set*Callback.
extern SkillSystem::NovaCallback        s_novaCallback;
extern SkillSystem::DashCallback        s_dashCallback;
extern SkillSystem::ScorchCallback      s_scorchCallback;
extern SkillSystem::DroneSpawnCallback  s_droneSpawnCallback;
extern SkillSystem::ChainCallback       s_chainCallback;
extern SkillSystem::BeamCallback        s_beamCallback;
extern SkillSystem::ReloadCallback      s_reloadCallback;

// Overcharged Magazine state — set by fireOverchargedMagazine (marksman),
// read by SkillSystem::isOvercharged / consumeOverchargeShot / tickOvercharge.
extern f32 s_overchargeTimer;
extern u8  s_overchargeShots;

// Holy Bombardment persistent state — set by fireHolyBombardment (paladin),
// ticked by SkillSystem::updateMeteors.
extern f32  s_bombardmentTimer;
extern Vec3 s_bombardmentCenter;
extern f32  s_bombardmentAccum;
extern f32  s_bombardmentDamage;
extern f32  s_bombardmentRadius;

// Holy Nova delayed second-hit state — set by fireHolyNova (paladin),
// ticked by SkillSystem::update.
extern f32  s_holyNovaTimer;
extern Vec3 s_holyNovaCenter;
extern f32  s_holyNovaDamage2;
extern f32  s_holyNovaRadius;
extern f32  s_holyNovaHealPct;

// ---------------------------------------------------------------------------
// Helper used by multiple families
// ---------------------------------------------------------------------------

// Rotate a direction vector by angleDeg degrees around the Y axis.
Vec3 rotateY(Vec3 v, f32 angleDeg);

// ---------------------------------------------------------------------------
// Legendary skill fire* functions  (skill_legendary.cpp)
// ---------------------------------------------------------------------------

void fireFrozenOrb(Vec3 origin, Vec3 direction, const SkillDef* def,
                   ProjectilePool& pool);

void fireChainLightning(Vec3 origin, Vec3 direction, const SkillDef* def,
                        const LevelGrid& grid, EntityPool& entities);

void fireBloodNova(Vec3 origin, const SkillDef* def, EntityPool& entities);

void fireMeteorStrike(Vec3 origin, Vec3 direction, const SkillDef* def,
                      const LevelGrid& grid);

void firePhaseDash(Vec3 eyePos, Vec3 forward, const SkillDef* def,
                   const LevelGrid& grid, EntityPool& entities, Player& player);

// ---------------------------------------------------------------------------
// Warrior skill fire* functions  (skill_warrior.cpp)
// ---------------------------------------------------------------------------

void fireCleave(Vec3 origin, Vec3 forward, const SkillDef* def,
                EntityPool& entities, Player& player);

void fireThunderclap(Vec3 origin, const SkillDef* def, EntityPool& entities);

void fireWarCry(Vec3 origin, const SkillDef* def, EntityPool& entities,
                Player& player);

void fireWhirlwind(Vec3 origin, const SkillDef* def, EntityPool& entities);

void fireEarthquake(Vec3 origin, const SkillDef* def, EntityPool& entities);

// ---------------------------------------------------------------------------
// Ranger skill fire* functions  (skill_ranger.cpp)
// ---------------------------------------------------------------------------

void fireMultiShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                   ProjectilePool& pool);

void fireRainOfArrows(Vec3 origin, Vec3 forward, const SkillDef* def,
                      const LevelGrid& grid, EntityPool& entities);

void firePoisonArrow(Vec3 origin, Vec3 forward, const SkillDef* def,
                     ProjectilePool& pool);

void fireShadowShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                    const LevelGrid& grid, EntityPool& entities);

void fireVolley(Vec3 origin, Vec3 forward, const SkillDef* def,
                const LevelGrid& grid, ProjectilePool& pool);

void firePiercingShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                      const LevelGrid& grid, EntityPool& entities);

void fireBarrage(Vec3 origin, Vec3 forward, const SkillDef* def,
                 ProjectilePool& pool);

bool fireMarkPrey(Vec3 origin, Vec3 forward, const SkillDef* def,
                  EntityPool& entities);  // false = no valid target (free, no cooldown)

// ---------------------------------------------------------------------------
// Sorcerer skill fire* functions  (skill_sorcerer.cpp)
// ---------------------------------------------------------------------------

void fireFireball(Vec3 origin, Vec3 forward, const SkillDef* def,
                  ProjectilePool& pool);

// ---------------------------------------------------------------------------
// Rogue skill fire* functions  (skill_rogue.cpp)
// ---------------------------------------------------------------------------

void fireKnifeBurst(Vec3 origin, Vec3 forward, const SkillDef* def,
                    ProjectilePool& pool);

void firePoisonCloud(Vec3 origin, Vec3 forward, const SkillDef* def,
                     const LevelGrid& grid, EntityPool& entities, Player& player);

bool fireShadowStrike(Vec3 origin, Vec3 forward, const SkillDef* def,
                      EntityPool& entities, Player& player);  // false = no target (free)

void fireFanOfKnives(Vec3 origin, Vec3 forward, const SkillDef* def,
                     ProjectilePool& pool, Player& player);

bool fireShadowStep(Vec3 origin, Vec3 forward, const SkillDef* def,
                    const LevelGrid& grid, EntityPool& entities, Player& player);  // false = no target/LOS (free)

void fireShadowDance(Player& player);

// ---------------------------------------------------------------------------
// Paladin skill fire* functions  (skill_paladin.cpp)
// ---------------------------------------------------------------------------

void fireHolySmite(Vec3 origin, Vec3 forward, const SkillDef* def,
                   EntityPool& entities, Player& player, const LevelGrid& grid);

void fireHolyBombardment(Vec3 origin, const SkillDef* def, Player& player);

void fireHolyNova(Vec3 origin, const SkillDef* def,
                  EntityPool& entities, Player& player);

void fireDivineJudgment(Player& player, EntityPool& entities, const SkillDef* def);

// ---------------------------------------------------------------------------
// Combat Engineer skill fire* functions  (skill_engineer.cpp)
// ---------------------------------------------------------------------------

void fireShockBolt(Vec3 origin, Vec3 forward, const SkillDef* def,
                   ProjectilePool& projectiles);

void fireDeployTurret(Vec3 origin, Vec3 forward, EntityPool& entities);

void fireTeslaCoil(Vec3 origin, const SkillDef* def,
                   EntityPool& entities, ProjectilePool& pool);

void fireMechOverdrive(Player& player);

// ---------------------------------------------------------------------------
// Marksman skill fire* functions  (skill_marksman.cpp)
// ---------------------------------------------------------------------------

void fireAimedShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                   const LevelGrid& grid, EntityPool& entities);

void fireExplosiveRound(Vec3 origin, Vec3 forward, const SkillDef* def,
                        const LevelGrid& grid, EntityPool& entities);

void fireOverchargedMagazine(Vec3 origin, Vec3 forward);

void fireHeadshot(Vec3 origin, Vec3 forward, const SkillDef* def,
                  const LevelGrid& grid, EntityPool& entities,
                  SkillState& skillState);

// ---------------------------------------------------------------------------
// Tinkerer skill fire* functions  (skill_tinkerer.cpp)
// ---------------------------------------------------------------------------

void fireCombatDrone(Vec3 origin, Vec3 forward, EntityPool& entities);

void fireSwarmDrones(Vec3 origin, Vec3 forward, EntityPool& entities);

void fireStunGrenade(Vec3 origin, Vec3 forward, const SkillDef* def,
                     ProjectilePool& pool);

void fireSwarmDeploy(Vec3 origin, Vec3 forward, EntityPool& entities);

void fireOverclock(Vec3 origin, const SkillDef* def, EntityPool& entities);

void fireDetonateSwarm(const SkillDef* def, EntityPool& entities);

void fireSwarmQueen(Vec3 origin, Vec3 forward);
