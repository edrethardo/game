#!/usr/bin/env bash
# cut_cinematic.sh — the CINEMATIC trailer's edit list (WB-273, epic WB-266).
# HUD-free takes only. Hard cuts; the chakram storm carries the finale (the epic's call:
# the signature shot opens OR closes — it closes, so the world builds toward it).
# Music is the PLACEHOLDER bed until the user picks the real track (WB-274).
source "$(dirname "$0")/cut_common.sh"

card $'CURSE OF THE\nDUNGEON ENGINE' 3.0 both
clip town_reveal    2.0 6.0
clip lava_glide_v2  2.0 7.5
clip take_stage     1.5 4.0
clip loot_shower    1.0 7.5
clip take_orbit     0.5 5.0
clip take_glide     0.5 6.0
card $'INFINITE\nCHAKRAMS' 2.0 both
clip chakram_orbit  0.5 11.5
card $'CURSE OF THE DUNGEON ENGINE\n\nWISHLIST ON STEAM' 3.5 both
finish "$TAKES/trailer_cinematic_v1.mp4"
