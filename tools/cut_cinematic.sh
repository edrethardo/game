#!/usr/bin/env bash
# cut_cinematic.sh — the CINEMATIC trailer's edit list (WB-273, epic WB-266). v6.
#
# v2 fixed the balance (biome tour, chakrams only as the finale); v3 swaps every text overlay for
# the OLD title sequence's art (store/trailer): the word-by-word CURSE/OF/THE/DUNGEON-ENGINE
# zoom-out opens, the gold-wordmark cards carry the beats, and the animated logo walk closes into
# the wishlist card. One visual language, first frame to last (the user's call — "die waren cooler").
source "$(dirname "$0")/cut_common.sh"
ST="$(dirname "$0")/../store/trailer"

pngcard "$ST/frame_1_curse.png" 0.8 in
pngcard "$ST/frame_2_of.png" 0.55
pngcard "$ST/frame_3_the.png" 0.55
pngcard "$ST/frame_4_dungeon_engine.png" 1.4
clip town_reveal      2.0 5.0
# v6: the dungeon/catacombs/void biome beats ride the _2 re-shoots on DEEPER floors of the
# same theme (7/18/47) — an endgame hero on floor 3 raced empty corridors, and an empty
# corridor is not a spectacle. Trims sit in each take's scanned clean windows.
clip biome_dungeon2   7.2 4.2
clip biome_cata2      4.0 4.2
clip biome_caverns   10.3 4.2
clip biome_descent    6.3 4.2
clip lava_glide_v2    3.0 5.5
clip biome_void2      2.8 4.2
# THE WEAPON MONTAGE (user: "show more weapon variety") — seven legendaries, one beat each, all
# IN ACTION on staged --hidehud bot runs: claymore sweep, scythe, bow, revolver, hellfire
# launcher, void flask fire, staff bolts. Trim windows sit inside each take's longest clean
# stretch (transition slates mapped by frame size — the couch lesson, third appearance).
# v7 of the montage (user: "abwechslungsreicher — mehr Biome"): every weapon fights in its
# OWN look — stone 3, catacombs 18, caverns 25, the two-story hall 8, the molten sea 33, the
# void 47, the Descent maze 9. The wpn3 takes stage the weapon PLUS a legendary armor kit
# (the orb recipe's survivability trick — a fresh unarmored hero dies in seconds past ~12).
clip wpn_claymore     4.2 2.2
clip wpn3_scythe      9.0 2.2
clip wpn3_bow         4.0 2.2
clip wpn3_revolver    7.0 2.2
clip wpn3_launcher    2.3 2.2
clip wpn3_flask       6.3 2.2
clip wpn3_staff       1.0 2.2
clip loot_shower      1.0 6.0
clip take_stage       1.5 3.0
# No INFINITE CHAKRAMS card (user: "bewerbe sie nicht — zeig sie in Action"): the weapon sells
# itself — one thrown disc, camera on its tail, a wall bounce, a kill (chakram_follow), then the
# full storm from the orbit.
clip chakram_follow   0.0 4.5
clip chakram_orbit    0.5 6.5
mp4seg "$ST/logo_walk.mp4"
pngcard "$ST/card_out_now.png" 3.0 out
finish "$TAKES/trailer_cinematic_v7.mp4"
