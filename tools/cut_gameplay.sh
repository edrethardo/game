#!/usr/bin/env bash
# cut_gameplay.sh — the GAMEPLAY trailer's edit list (WB-273, epic WB-267).
# HUD-ON takes only, honest footage: bot-played runs and staged-but-real fights. Title
# cards carry the numbers; the death screen is the honest beat before the end card.
source "$(dirname "$0")/cut_common.sh"

card "ACTUAL GAMEPLAY" 1.6 in
clip dungeon_run     4.0 4.0
clip zone_action     4.0 4.0
card "9 CLASSES" 1.3
clip class_tinkerer  6.0 3.5
clip class_engineer  0.2 3.5
clip minion_army     5.0 3.0
card "ACT BOSSES" 1.3
clip boss_griswald   4.0 4.0
clip merge_split     5.0 3.5
card "INFINITE CHAKRAMS" 1.3
clip chakram_player  2.0 4.0
clip menu_beat       1.0 2.0
card "COUCH CO-OP" 1.3
# 10.5, not 4.0: the endgame bots blast through floor 1 in ~4 s, so the early window is the
# floor-transition SLATE, not split-screen play (caught in QC frame game_40).
clip couch_coop     10.5 3.5
clip tunnel_check    3.3 1.6
card "WISHLIST ON STEAM" 2.5 both
finish "$TAKES/trailer_gameplay_v1.mp4"
