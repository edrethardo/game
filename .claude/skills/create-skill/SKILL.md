---
name: create-skill
description: Use when adding a new activated player skill (legendary/class) to DungeonEngine — the SkillId enum value, SkillDef fields, skills.json entry, the tryActivate fire branch, optional per-tick logic, the generated HUD icon, and a legendary item that grants it. Trigger for "add a skill", "new legendary ability", "a spell that does X".
---

# Create a new (legendary) skill

End-to-end workflow for one new activated skill. A skill is **dispatched by `SkillId`** through
`SkillSystem::tryActivate`, and reaches the player by being the `legendarySkill` on an equipped
legendary weapon. Reference material lives in the `engine-reference` skill.

## Step 1 — Pin the concept

Name, cooldown, energy cost (or health cost like Blood Nova), base damage/radius, and the
*shape* of the effect: **instantaneous** (fire-and-forget projectiles/raycast/AoE) vs
**persistent** (needs per-tick updates — orb shards, delayed meteors, channeled zones).
Check `MAX_SKILL_DEFS` (`src/game/item.h`, 64) for headroom.

## Step 2 — Apply the code edits

Apply `templates/skill_snippets.md` — the ordered edits with exact insertion points and
exemplars (`fireChainLightning`/`fireMeteorStrike` in `src/game/skill_legendary.cpp` are the
patterns to copy):
1. `SkillId` enum value — `src/game/item.h` (~line 77, before `COUNT`; **append** — order is
   wire-significant).
2. New `SkillDef` fields (only if your skill needs params beyond the common ones) — same file
   (~line 297).
3. Parse those fields + map the id string in `loadSkillDefs` / `skillIdFromString` —
   `src/game/item_loader.cpp`.
4. A `fire<Name>()` function — `src/game/skill_legendary.cpp` (+ declaration in
   `src/game/skill_internal.h`).
5. A `case SkillId::<NAME>:` in `SkillSystem::tryActivate` — `src/game/skill_system.cpp`
   (~line 320). Add sound + particles cases too so it isn't silent.
6. **If persistent:** tick state in `SkillSystem::update`/`updateMeteors`/`updateOrbProjectiles`.

## Step 3 — skills.json entry

Append the filled-in `templates/skill_entry.json` to the `"skills"` array in
`assets/config/skills.json`. `id` must equal your `skillIdFromString` string. Include any
skill-specific fields you added in Step 2.4 (the loader reads them by exact name).

## Step 4 — HUD icon

Add an icon so the skill bar isn't blank:
1. Add an `icon_<name>()` function + an entry in the `icons` list in `tools/gen_skill_icons.py`
   (model on `icon_chain_lightning`). The list name is CamelCase (e.g. `InfernoBurst`).
2. Regenerate the header: `python3 tools/gen_skill_icons.py` → `src/renderer/skill_icons_data.h`.
3. Wire it in `src/renderer/hud_skill_bar.cpp`: a `case SkillId::<NAME>: return &kIcon32_<Name>[0][0];`
   in `getSkillIcon`, and a palette case in `getSkillIconColors`.

## Step 5 — A legendary item that grants it

A skill is inert until a legendary weapon carries it. Add (or reuse) an `items.json` weapon with
`"legendarySkill": "<id>"` (see the `create-weapon` skill). When equipped as LEGENDARY it
becomes the player's right-click skill.

## Step 6 — Build + compile

```bash
python3 tools/gen_skill_icons.py     # regenerate the icon header
cmake --build build
```
Fix errors. **Stop here** (build + compile only). *(Optional manual check: equip the granting
weapon and right-click to fire.)*

## Step 7 — Document

Inline-comment the fire logic (the *why*). Add the new skill to the skill list in the
`engine-reference` skill per the doc-sync rule.

## Gotchas

- **Five things must agree on the id:** the `SkillId` enum, `skillIdFromString`, the skills.json
  `"id"`, the icon list name, and the granting item's `legendarySkill`.
- **Append to the enum** — inserting in the middle shifts values and breaks saves/wire order.
- `tryActivate` checks resource availability *then* commits cost only on real activation —
  return the "didn't fire" path for whiffs instead of charging cooldown.
- Persistent skills use file-scope pools/timers in `skill_system.cpp` (e.g. `s_meteors`) ticked
  by the update loops — don't try to do multi-frame work inside `fire<Name>()`.
