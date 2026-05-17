# Enemy Roster & Boss Rework Design

**Date:** 2026-05-17
**Status:** Draft

## Context

The game has a rich combat system (60+ skills, 120+ weapons, 8 classes) but only 3 genuinely distinct enemy types. The current 5-tier roster (6-8 per tier) is mostly stat reskins of skeleton/bat/spider with different on-hit effects. Boss AI is placeholder — all 10 bosses use the same chase/attack FSM as regular enemies with no unique behavior. Boss names are Diablo clones. Combat gets repetitive because there's nothing interesting to fight.

This rework replaces the entire enemy roster with 37 behaviorally distinct types and redesigns all 10 bosses with unique AI personalities, role combinations, and skill usage.

## Architecture

### Core Principle: Unified System

Bosses and regular enemies are the same system. The difference is stats, role combinations, AI personality, and skill access. No boss-specific AI code paths — everything flows through the role-based FSM.

### Key Changes

1. **`EnemyRole` becomes a bitmask** — regular enemies get one role, bosses can stack 2-3
2. **New roles added:** `RANGED_CASTER`, `CHARGER`, `BOMBER`, `SHIELD_BEARER` (alongside existing NORMAL, AMBUSH, SUMMONER, HEALER, AURA)
3. **New `BossPersonality` enum** — named AI behavior patterns that override FSM state selection:
   - `BERSERKER` — never retreats, always charges shortest path, escalation extra aggressive
   - `KITER` — maintains max preferred range, retreats aggressively, hides behind summons
   - `TELEPORTER` — periodically blinks to random position near player, unpredictable
   - `DUELIST` — circle-strafes, punishes player attacks, prefers 1v1 range
4. **Boss skill slots** — bosses can cast skills from `skills.json` (same system players use)
5. **Enrage curve** — continuous escalation via `enrageFactor` JSON field. As HP drops, attack cooldowns shorten, move speed increases, summoners spawn faster. Formula: `mult = 1.0 + (1.0 - hpPercent) * enrageFactor`
6. **Minion shield** — optional JSON flag. Boss takes reduced damage while minions are alive
7. **Role synergies** — implicit in the role system:
   - Summoner + Aura: boss buffs own summons
   - Charger + Healer: lifesteal on charge hits
   - Summoner + Healer: boss heals own summons
8. **Guaranteed boss loot** — mini-bosses drop rare+, major bosses drop legendary
9. **`bosses.json`** — new config file for boss definitions
10. **`enemies.json` promoted** — replaces inline `kTier*` arrays as single source of truth

### Boss Skill Execution

Bosses cast skills from `skills.json` using a new `BossSkillSystem::tryCast(Entity&, SkillDef&, ...)` function. This mirrors `SkillSystem::tryActivate` but sources position/facing from the Entity instead of Player. The skill's projectile/AoE/effect code is shared — only the activation trigger differs. Boss skill cooldowns use the existing `Entity.attackCooldown` timer (one skill at a time, interleaved with normal attacks).

### Shield Bearer Mechanic

Frontal damage reduction is determined by dot product: `dot(damageDirection, entityFacing) < -0.3` means the attack hit the front. Front hits take 50% reduced damage. This encourages flanking. Shield Bearers turn to face the player, so you need to circle-strafe or use multiple angles in co-op.

### Teleport Mechanic

Used by Teleporter personality (bosses) and Shade enemy type. Implementation: pick a random walkable cell within 8m of the target, verify line-of-sight to target from new position, snap entity position. No pathfinding needed. Bosses teleport on a timer (every 4-6s). Shades teleport when transitioning from IDLE/FLANK to CHASE (ambush teleport). Brief particle burst at origin and destination for player readability.

### Entity Struct Changes

- Add `u8 bossDefIdx = 0xFF` (1 byte) — index into boss def array, 0xFF = not a boss
- Change `EnemyRole` from `enum struct : u8` to `u8` bitmask (same size, 8 role bits)
- Total: 1 byte added to Entity struct

### New Files

- `src/game/boss_def.h` — `BossDef` struct, `BossPersonality` enum, `MAX_BOSS_DEFS = 16`
- `src/game/boss_ai.cpp` — Boss AI personality dispatch (called from `enemy_ai.cpp` when `enemyType == BOSS`)
- `assets/config/bosses.json` — Boss definitions

### Modified Files

- `src/game/entity.h` — `EnemyRole` bitmask, new role values, `bossDefIdx` field on Entity
- `src/game/enemy_ai.cpp` — Role-based state selection in CHASE/ATTACK handlers, boss AI delegation
- `src/engine/engine_startgame.cpp` — Replace `kBosses` and `kTier*` inline arrays with JSON-loaded defs
- `src/engine/engine_init.cpp` — Load `bosses.json`, guaranteed boss loot in death callback
- `src/game/item.h` / `item.cpp` — Boss loot guarantee (force minimum rarity)
- `assets/config/enemies.json` — Expanded schema with `role`, `aiPreference` fields

---

## Boss Roster (10 bosses)

### Tier 1 — Stone Dungeon (warm torchlight, medieval stone)

**Floor 5 (mini): The Butcher**
- Roles: Charger + Ranged Caster
- AI Personality: Berserker
- Weapon: Cleaver (melee + thrown projectile using weapon mesh)
- Skills: None
- On-hit (cleaver throw): Slow (1 second)
- Stats: 800 HP, 80 DMG, speed 3.0
- Mesh: butcher
- Gimmick: Charges you in melee, throws his cleaver when you run. Cleaver throw slows on hit so he closes the gap. Gets faster as HP drops via enrage. No safe distance.

**Floor 10 (major): Ygara, the Broodqueen**
- Roles: Summoner + Aura
- AI Personality: Kiter
- Weapon: None (melee bite)
- Skills: Poison Nova
- On-hit: Poison
- Stats: 1000 HP, 65 DMG, speed 4.0
- Mesh: andariel (spider legs limb config)
- Minion shield: Yes
- Gimmick: Spider matriarch who floods the arena with spiderlings while buffing them with her aura. Kites to maintain distance. Must thin the brood to damage her.

### Tier 2 — Catacombs (sickly green light, mossy tombs)

**Floor 15 (mini): Sethrak the Undying**
- Roles: Summoner + Ranged Caster
- AI Personality: Kiter
- Weapon: Staff
- Skills: Chain Lightning
- On-hit: Poison
- Stats: 500 HP, 30 DMG, speed 2.8
- Mesh: skeleton (lich material)
- Gimmick: Fast necromancer who kites while raising the dead and zapping you with Chain Lightning. Kill the summons or you'll be overwhelmed.

**Floor 20 (major): Malachar, Warden of Tombs**
- Roles: Aura + Ranged Caster
- AI Personality: Teleporter
- Weapon: Staff
- Skills: Frozen Orb
- On-hit: Slow
- Stats: 1200 HP, 30 DMG, speed 2.5
- Mesh: skeleton (tentacle limb config)
- Gimmick: Tentacled tomb guardian who blinks between positions, fires Frozen Orbs, and his aura slows you. Unpredictable positioning forces constant movement.

### Tier 3 — Spider Caverns (crystal blue light, dark purple stone)

**Floor 25 (mini): Ixara, Cavern Mother**
- Roles: Summoner + Healer
- AI Personality: Kiter
- Weapon: None (melee bite)
- Skills: None
- On-hit: Slow
- Stats: 700 HP, 30 DMG, speed 5.0
- Mesh: spider (spider queen material)
- Gimmick: Fast spider who spawns broodlings AND heals them. You have to burst her down or the adds become unkillable.

**Floor 30 (major): Korvath, the Siege Lord**
- Roles: Charger + Shield Bearer
- AI Personality: Berserker
- Weapon: None (fists/slam)
- Skills: Earthquake
- On-hit: Slow
- Stats: 1800 HP, 30 DMG, speed 3.0
- Mesh: butcher (baal material)
- Gimmick: Massive war demon who charges and has frontal damage reduction. Uses Earthquake on arrival. You have to flank him — fighting head-on is suicide.

### Tier 4 — Hellforge (lava red light, demonic forges)

**Floor 35 (mini): Azhar, the Ashen Blade**
- Roles: Charger + Ranged Caster
- AI Personality: Duelist
- Weapon: Sword
- Skills: Fireball
- On-hit: Burn
- Stats: 800 HP, 25 DMG, speed 3.5
- Mesh: butcher (demon knight material)
- Gimmick: Flame knight who alternates between sword charges and ranged fireballs. Circle-strafes and punishes missed attacks. Burn on-hit.

**Floor 40 (major): Diablo**
- Roles: Charger + Aura
- AI Personality: Berserker
- Weapon: Sword
- Skills: Meteor Strike
- On-hit: Burn
- Stats: 1600 HP, 30 DMG, speed 3.5
- Mesh: butcher (diablo material, back spikes limb config)
- Gimmick: Lord of the Hellforge. Charges with a burn aura that damages nearby players, calls down Meteor Strikes. Relentless pressure — you have to kite a boss that doesn't want to be kited.

### Tier 5 — Void (pale purple light, cosmic darkness)

**Floor 45 (mini): Nyx, Weaver of the Void**
- Roles: Ranged Caster + Summoner
- AI Personality: Teleporter
- Weapon: Staff
- Skills: Frozen Orb
- On-hit: Freeze
- Stats: 600 HP, 20 DMG, speed 3.0
- Mesh: skeleton (arch mage material)
- Gimmick: Void sorcerer who blinks constantly, fires Frozen Orbs, and summons void shades. Freeze on-hit. Chaotic fight — hard to pin down.

**Floor 50 (major): Grim Reaper**
- Roles: Summoner + Aura + Charger
- AI Personality: Berserker
- Weapon: Axe
- Skills: Blood Nova
- On-hit: Freeze
- Stats: 2500 HP, 30 DMG, speed 4.0
- Mesh: skeleton (reaper material, blade arms limb config)
- Minion shield: Yes
- Enrage factor: High
- Gimmick: Death incarnate. Summons void skeletons, buffs them with freeze aura, charges with axe. Blood Nova devastates the arena. Everything the game taught you comes together.

---

## Regular Enemy Roster (37 types across 5 tiers)

### Tier 1 — Stone Dungeon (floors 1-10)
*Tutorial tier. Each enemy teaches one mechanic.*

| # | Name | Role | AI Preference | Flying | On-Hit | Behavior |
|---|---|---|---|---|---|---|
| 1 | Shambling Corpse | Normal | CHASE | no | — | Slow, predictable melee. Teaches basic combat. Low threat alone, dangerous in groups. |
| 2 | Bone Archer | Ranged Caster | STRAFE | no | — | Fires bone projectiles, sidesteps between shots. Teaches "close the gap on ranged enemies." |
| 3 | Carrion Bat | Normal | FLYBY | yes | — | Swoops in for hit-and-run. Teaches tracking airborne targets. |
| 4 | Dungeon Spider | Normal | CHASE | no | Poison | Fast melee, poison on hit. Teaches "prioritize fast enemies + manage DoT." |
| 5 | Tomb Gargoyle | Ambush | DORMANT | no | — | Disguised as prop, bursts with high damage. Teaches awareness and checking corners. |
| 6 | Wretched Imp | Bomber | FLYBY/RETREAT | yes | — | Tiny flying nuisance, dive-bombs then retreats. Low HP. Teaches snap-aiming. |

### Tier 2 — Catacombs (floors 11-20)
*Introduces support roles. Enemies work together.*

| # | Name | Role | AI Preference | Flying | On-Hit | Behavior |
|---|---|---|---|---|---|---|
| 1 | Catacomb Sentinel | Shield Bearer | SURROUND | no | Poison | Frontal damage reduction, slow, tanky. Forces flanking. |
| 2 | Ghoul | Charger | CHASE/RETREAT | no | Poison | Sprints in, hits hard, retreats. Hit-and-run teaches timing. |
| 3 | Bone Mage | Ranged Caster | STRAFE | no | Poison | Poison projectiles, kites. Priority target — kill the caster first. |
| 4 | Plague Bat | Bomber | FLYBY | yes | Poison | Explodes into poison cloud on death. Teaches "don't melee everything." |
| 5 | Necromancer | Summoner | STRAFE/RETREAT | no | — | Resurrects dead enemies. Teaches "kill the summoner first or the fight never ends." |
| 6 | Tomb Wraith | Normal | FLANK | no | — | Flanks to get behind you, short stun on hit. Teaches watching your back. |
| 7 | Crypt Herald | Aura | SURROUND | no | Poison | Buffs nearby enemies. Teaches "kill the buffer." |

### Tier 3 — Spider Caverns (floors 21-30)
*Crowd control tier. Enemies restrict your movement.*

| # | Name | Role | AI Preference | Flying | On-Hit | Behavior |
|---|---|---|---|---|---|---|
| 1 | Cavern Stalker | Charger | FLANK/CHASE | no | Slow | Fast humanoid, flanks to rear, backstab bonus. Punishes tunnel vision. |
| 2 | Broodmother | Summoner | RETREAT | no | Slow | Large spider, spawns spiderlings on a timer. War of attrition if ignored. |
| 3 | Web Spinner | Bomber | STRAFE | no | Slow | Fires slow projectiles that create slow-fields on impact. Area denial. |
| 4 | Sniper Imp | Ranged Caster | STRAFE/RETREAT | yes | — | Long range, high damage, slow fire rate. Forces closing distance or using cover. |
| 5 | Cavern Shaman | Healer | RETREAT | no | — | Heals lowest-HP ally. Must be prioritized or fights drag. |
| 6 | Cavern Herald | Aura | SURROUND | no | Slow | Buffs pack. Pairs with Broodmother for nightmare combos. |
| 7 | Cave Troll | Shield Bearer | CHASE | no | Slow | Massive HP, slow. Ground-slam AoE on death. Don't be near when it drops. |
| 8 | Burrowing Widow | Ambush | DORMANT/CHASE | no | Poison | Disguised as floor prop, erupts when stepped on. Fast but fragile. |

### Tier 4 — Hellforge (floors 31-40)
*Aggression tier. Enemies punish passivity.*

| # | Name | Role | AI Preference | Flying | On-Hit | Behavior |
|---|---|---|---|---|---|---|
| 1 | Hellhound | Charger | CHASE | no | Burn | Sprint-charges, leaves burn trail. Fast and relentless. |
| 2 | Demon Caster | Ranged Caster | STRAFE | no | Burn | Fire projectiles with splash. Area denial from range. |
| 3 | Flame Imp | Bomber | FLYBY/RETREAT | yes | Burn | Dive-bombs with splash, retreats. Suicide bomber if low HP. |
| 4 | Pit Fiend | Shield Bearer | CHASE | no | Burn | Massive demon, frontal reduction. Throws boulders if kited. Ranged + tanky. |
| 5 | Infernal Herald | Aura | SURROUND | no | Burn | Burn aura damages you by proximity. Forces snipe or rush. |
| 6 | Hellforge Smith | Summoner | RETREAT | no | — | Summons fire elementals. Stays back, hard to reach. |
| 7 | Succubus | Ranged Caster | FLANK | yes | — | Flying, flanks constantly, stun on hit. Disorienting — hard to track. |

### Tier 5 — Void (floors 41-50)
*Everything you learned, weaponized. Every enemy is dangerous.*

| # | Name | Role | AI Preference | Flying | On-Hit | Behavior |
|---|---|---|---|---|---|---|
| 1 | Void Revenant | Charger | CHASE | no | Freeze | Relentless, freeze on hit locks you for followup from others. |
| 2 | Shade | Normal | FLANK | no | Freeze | Teleport-flanks (skips pathfinding, appears near you). Disorienting. |
| 3 | Entropy Weaver | Ranged Caster + Bomber | STRAFE | no | Freeze | Void projectiles that leave persistent damage zones. Arena becomes minefield. |
| 4 | Void Necromancer | Summoner | RETREAT | no | Freeze | Resurrects dead as void versions with freeze. Adds more dangerous than originals. |
| 5 | Void Shaman | Healer | RETREAT | no | Freeze | Heals allies AND applies freeze aura to healed targets. |
| 6 | Void Herald | Aura | SURROUND | no | Freeze | Freeze aura slows you when near. Combined with Void Revenant = death. |
| 7 | Nullifier | Bomber | FLYBY/RETREAT | yes | — | On-death explosion suppresses player skills for 3s. Forces weapon-only combat. |
| 8 | Abyssal Titan | Shield Bearer | CHASE | no | Freeze | Massive void tank. Frontal immune. Ground slam freeze AoE. Cave Troll's final form. |

---

## Boss Loot System

### Guaranteed Drops
- **Mini-bosses** (floors 5, 15, 25, 35, 45): guaranteed Rare+ quality drop
- **Major bosses** (floors 10, 20, 30, 40, 50): guaranteed Legendary quality drop

### Implementation
In the death callback (`engine_init.cpp`), before normal loot logic:
- Check `entity.bossDefIdx != 0xFF`
- Look up `BossDef.lootGuarantee` (Rarity enum)
- Roll item via `ItemGen::rollItem`, then force rarity up to minimum if rolled lower
- Re-roll affixes to match new rarity tier
- Spawn via `WorldItemSystem::spawn`
- Major bosses also drop 1-2 bonus items at normal rarity
- Skip normal loot path (return early)

---

## AI Personality System

### BossPersonality Enum
```
BERSERKER   — Never retreats. Always picks shortest-path CHASE. Enrage curve applies extra aggressively.
KITER       — Maintains preferred range (high value). Switches to RETREAT if player closes. Hides behind summons.
TELEPORTER  — Every N seconds, blinks to random walkable position within range of player. Resets engagement.
DUELIST     — Circle-strafes via STRAFE state. After player attacks (miss window), counter-charges briefly.
```

### Integration with FSM
In `enemy_ai.cpp`, when `enemyType == BOSS`, the state-selection logic checks `BossPersonality` before choosing the next AI state. For example:
- A BERSERKER boss in CHASE never transitions to RETREAT regardless of HP
- A KITER boss in CHASE checks distance and transitions to RETREAT if player is within preferred range
- A TELEPORTER boss has a `teleportTimer` that triggers a position blink periodically
- A DUELIST boss tracks player attack timing and transitions to CHARGE during recovery frames

### Enrage Curve
Applied per-tick to boss stats:
```
enrageMult = 1.0 + (1.0 - currentHp / maxHp) * enrageFactor
effectiveCooldown = baseCooldown / enrageMult
effectiveSpeed = baseSpeed * enrageMult
```
Configurable per boss via `enrageFactor` in `bosses.json`. Default 0.3 (30% faster at 0 HP). The Grim Reaper might use 0.5 for extreme escalation.

---

## Role Bitmask System

### New EnemyRole Definition
```
ROLE_NORMAL       = 0x0000
ROLE_AMBUSH       = 0x0001
ROLE_SUMMONER     = 0x0002
ROLE_HEALER       = 0x0004
ROLE_AURA         = 0x0008
ROLE_RANGED_CASTER = 0x0010
ROLE_CHARGER      = 0x0020
ROLE_BOMBER       = 0x0040
ROLE_SHIELD_BEARER = 0x0080
```

### Role Synergies (implicit)
When multiple roles are active on the same entity:
- **Summoner + Aura**: Aura applies to own summons (already works via proximity check)
- **Summoner + Healer**: Boss heals own summons (healer targets lowest-HP ally, summons qualify)
- **Charger + Aura**: Charge brings aura into melee range, buffing nearby allies on arrival
- **Shield Bearer + Charger**: Frontal reduction during charge — must sidestep, can't face-tank

### Minion Shield
Optional per-boss flag in `bosses.json`. When active:
- Boss takes 75% reduced damage while any minion spawned by this boss is alive
- Visual indicator on boss (damage numbers show reduced values)
- Forces player to clear adds before focusing boss
- Used by: Ygara (floor 10), Grim Reaper (floor 50)

---

## JSON Schema Changes

### bosses.json (new file)
```json
{
  "bosses": [
    {
      "name": "The Butcher",
      "floor": 5,
      "isMajor": false,
      "baseHp": 800,
      "baseDmg": 80,
      "moveSpeed": 3.0,
      "attackRange": 3.5,
      "attackCooldown": 1.0,
      "detectionRange": 40.0,
      "meshName": "butcher",
      "materialName": "butcher_skin",
      "weaponType": "melee",
      "weaponMesh": "cleaver",
      "halfExtents": [0.5, 1.0, 0.5],
      "roles": ["charger", "ranged_caster"],
      "personality": "berserker",
      "skills": [],
      "enrageFactor": 0.3,
      "minionShield": false,
      "onHitEffect": "none",
      "projectile": {
        "enabled": true,
        "usesWeaponMesh": true,
        "speed": 18.0,
        "radius": 0.15,
        "cooldown": 4.0,
        "onHitEffect": "slow",
        "onHitDuration": 1.0
      },
      "lootGuarantee": "rare",
      "bonusDrops": 0,
      "limbConfig": 0,
      "speech": "FRESH MEAT!"
    }
  ]
}
```

### enemies.json (expanded schema)
```json
{
  "enemies": [
    {
      "name": "Shambling Corpse",
      "tier": 1,
      "meshName": "skeleton",
      "materialName": "zombie_skin",
      "health": 70,
      "moveSpeed": 1.8,
      "detectionRange": 12,
      "attackRange": 2.5,
      "attackCooldown": 1.2,
      "damage": 13,
      "flying": false,
      "halfExtents": [0.4, 0.9, 0.4],
      "role": "normal",
      "aiPreference": "chase",
      "onHitEffect": "none",
      "onHitDuration": 0,
      "onHitDps": 0,
      "dropWeight": 1.0
    }
  ]
}
```

---

## Difficulty Curve Summary

| Tier | Floors | Theme | Enemy Count | Teaching Focus |
|---|---|---|---|---|
| 1 | 1-10 | Stone Dungeon | 6 types | One mechanic per enemy, solo/pairs |
| 2 | 11-20 | Catacombs | 7 types | Support roles, target prioritization |
| 3 | 21-30 | Spider Caverns | 8 types | Crowd control, positioning, area denial |
| 4 | 31-40 | Hellforge | 7 types | Aggression, punishes passivity |
| 5 | 41-50 | Void | 8 types | All mechanics combined, every enemy dangerous |

Boss difficulty mirrors this: early bosses test one skill, late bosses test everything.
