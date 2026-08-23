#!/usr/bin/env bash
# cut_gameplay.sh — the GAMEPLAY trailer's edit list (WB-273, epic WB-267). v7.
#
# v7 (user): NO text cards mid-stream — hard cuts, denser pace. The class section is four
# SKILL SHOWCASES (frozen orbs, drone army, turret squad, Divine Judgment), the boss beat is
# a six-boss REEL at 2 s each (the three v6 bosses plus Butcher/Malachar/Grim Reaper). Only
# the OUT NOW end card survives — it is the sender and the call to action, not a caption.
source "$(dirname "$0")/cut_common.sh"
ST="$(dirname "$0")/../store/trailer"

clip g_orb        4.2 4.0
clip g_lava       8.5 3.0
clip g_vhall      21.3 3.0
clip g_tinkerer2  19.4 3.5
clip g_engineer2  10.0 3.5
clip g_paladin    16.6 3.5
clip g_descent    7.0 3.0
# The boss reel, ascending floors: Butcher 5 (the DUEL take — armor-only kit, starter sword,
# so he stays alive and ON CAMERA), Ygara 10, Malachar 20, Azhar 35, DiaBRO 40, Nyx 45 —
# two seconds each. Grim Reaper was tried and dropped: Inferno 50 killed the marksman three
# times and the boss never reached the frame.
clip g_butcher    5.4 2.0
clip g_ygara      13.2 2.0
clip g_malachar   8.3 2.0
clip g_azhar      13.8 2.0
clip g_diabro     6.4 2.0
clip g_nyx        28.2 2.0
clip g_chakram    4.0 4.0
clip g_couch      16.0 3.0
clip g_death      8.5 1.5
pngcard "$ST/card_out_now.png" 2.5 out
finish "$TAKES/trailer_gameplay_v7.mp4"
