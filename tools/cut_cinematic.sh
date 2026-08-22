#!/usr/bin/env bash
# cut_cinematic.sh — the CINEMATIC trailer's edit list (WB-273, epic WB-266). v3.
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
clip biome_dungeon    0.3 4.2
clip biome_catacombs  4.0 4.2
clip biome_caverns    4.0 4.2
clip biome_descent    4.0 4.2
clip lava_glide_v2    3.0 5.5
clip biome_void       4.0 4.2
# THE WEAPON MONTAGE (user: "show more weapon variety") — seven legendaries, one beat each, all
# IN ACTION on staged --hidehud bot runs: claymore sweep, scythe, bow, revolver, hellfire
# launcher, void flask fire, staff bolts. Trim windows sit inside each take's longest clean
# stretch (transition slates mapped by frame size — the couch lesson, third appearance).
clip wpn_claymore     6.0 2.2
clip wpn_scythe       3.0 2.2
clip wpn_bow          3.0 2.2
clip wpn_revolver     2.5 2.2
clip wpn_launcher     1.2 2.2
clip wpn_flask        2.5 2.2
clip wpn_staff        4.0 2.2
clip loot_shower      1.0 6.0
clip take_stage       1.5 3.0
# No INFINITE CHAKRAMS card (user: "bewerbe sie nicht — zeig sie in Action"): the weapon sells
# itself — one thrown disc, camera on its tail, a wall bounce, a kill (chakram_follow), then the
# full storm from the orbit.
clip chakram_follow   0.0 4.5
clip chakram_orbit    0.5 6.5
mp4seg "$ST/logo_walk.mp4"
pngcard "$ST/card_out_now.png" 3.0 out
finish "$TAKES/trailer_cinematic_v5.mp4"
