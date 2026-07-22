# Changelog

Player-facing notes for DungeonEngine. Format follows [Keep a Changelog](https://keepachangelog.com);
versions are the `v*` git tags that CI builds from (see `docs/DEPLOYMENT.md`).

Entries describe what *changed for the player*. Internal refactors, test-only commits and doc syncs
are omitted unless they change behaviour. Generated with the `changelog` skill.

## [Unreleased]

Levels grow a third dimension: floors that stack, drop and burn.

> **Multiplayer compatibility break.** `PROTOCOL_VERSION` is now **24**. Clients on an older build
> cannot join this one and are rejected cleanly at the join screen — everyone must update together.
> **Saves are unaffected**: existing characters load normally.

### Added

- **"The Descent" — four-story maze floors.** Every floor ending in 9 (9, 19, 29, 39, 49) is a
  labyrinth stacked four stories deep on one footprint. The maze walls are shared by all four levels,
  so each story is the same layout re-read at a new height. You enter at the top and must reach the
  exit diagonally opposite on the ground floor, and the only way down is through the floor:
  - **Drop holes** (2+ cells across) can't be jumped — a committed fall of exactly one story.
  - **Jump gaps** (1 cell) can — clear them, or blow the timing and lose a story.
  - **Return pads** under about a third of the holes fling you back up through the hole you fell
    through, so a mistake costs time rather than the run.
  - Ways down get scarcer the deeper you go, so the last descent is a hunt.
  - Ranged enemies hold the hole edges and plunge-fire at you one level down.
- **"The Stacked Loop" — two-story floors.** A loop of nine areas laid out 3×3 across two stories:
  ground rooms at the corners, walk-on/walk-under balconies at the mid-sides, and an open central
  void every balcony overlooks and drops into. Ramps climb, catwalks cross (one intact, one with a
  jump gap), jump pads fling you back up. Spawn and exit sit on opposite sides *and* opposite
  stories, so every floor forces a full traversal. Appears on floor-6+ non-boss floors.
  - **Interior walls close the sightlines.** The ground story is no longer an open hall: each
    corner room is entered through a doorway (or under a ramp archway), and free-standing cover
    walls break up the corners and the central void. The balconies above are untouched — the
    overlook into the void is still the point.
  - **Both stories fight back.** Each balcony is held by a pack of snipers *and* melee guards
    (the spawn-side balcony stays clean), and the ground story carries full-density packs in
    every area instead of a handful — roughly four times the enemies overall.
  - Fixed: on the current larger layout, all four ramps ended just short of their balcony — a
    dead-end in the air. Every ramp now lands on its balcony.
- **Hellforge lava floors.** A few floors in the 31–40 tier now *melt*: every interior wall becomes
  molten rock, leaving islands of stone in a lava sea with nowhere to hide. Lava deals heavy damage
  **and sets you on fire**, so the burn keeps ticking after you scramble out — but monsters wade
  through it untouched and use it to flank you. Narrow veins can be jumped; wide lakes can't.
  Stepping-stone causeways cross the sea as optional shortcuts, and the minimap marks lava in orange.
- **Auto Loot & Equip — a second way to play.** Chosen at character creation (and toggleable any
  time in the inventory): the game plays your inventory for you. Nearby loot is vacuumed up, and a
  3×3 build grid (Tanky/Moderate/Glass Cannon × Magic/Melee/Ranged) decides what gets worn — pick a
  cell and the whole bag re-gears on the spot. The bag keeps best-in-slot gear for *all nine*
  builds, leaves worse loot on the ground, quietly discards dominated pieces (announced in chat),
  nudges you when another build could field better gear, and a full bag drops its least useful item
  instead of stopping. **Minipets are always grabbed and never discarded** — the auto-looter treats
  them as treasure, not gear. Classic mode is the game exactly as it was.

### Changed

- **Monsters hit harder and last longer: +50% base health and damage across the entire roster**
  (all 38 enemy types, every tier's fallback spawns, and mimics). Floor and difficulty scaling
  stack on top as before, so the whole curve shifts up. Bosses are unchanged.

- **More enemies on screen.** The entity budget rose from 128 to 192, so the four-story floors can be
  populated at full density on every level instead of crowding out decorations, NPCs and summons.
- **Jump pads are brighter and can be stronger.** Pads now glow vividly enough to spot across a dark
  room, and a level can author its own launch strength — the Descent's lift you about two stories.
  Arena pads keep their existing feel.

---

*No **Fixed** section this release: the four-story, two-story and lava floors are all new, so the
bugs found while building them were never present in a released build.*

Older releases predate this file; see the auto-generated notes on each GitHub Release.
