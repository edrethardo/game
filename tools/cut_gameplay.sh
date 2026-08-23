#!/usr/bin/env bash
# cut_gameplay.sh — the GAMEPLAY trailer's edit list (WB-273, epic WB-267). v9.
#
# v8 (user): "fast nur melee" / "man sieht die skills gar nicht richtig" — the cut now walks
# the EQUIPMENT spectrum (staff, bow, revolver, launcher, flask, guns, discs, sword) and every
# class beat is a SKILL showcase with its cast window pinned by log_time.py. Cardless since
# v7; the OUT NOW end card is the sender, not a caption.
source "$(dirname "$0")/cut_common.sh"
ST="$(dirname "$0")/../store/trailer"

clip g_orb        4.2 4.0
# Ranger over the molten sea: Volley (80 arrows) + Piercing Shot + Barrage back to back —
# staged Void Bow + armor kit, NO --endgame (the endgame roll handed the ranger a WAND).
clip g_lava_ranger 6.8 3.0
# v9: every weapon in its CLASS's hands — marksman/revolver (his START weapon), combat
# engineer/launcher, tinkerer/flask — plus the two classes no cut had shown: the rogue's
# 144-knife Fan of Knives with the Stiletto's ricochet procs, and the wanderer's
# Adrenaline-Surge melee massacre. All nine classes are now in the trailer.
clip gw_revolver  1.8 2.2
clip gw_launcher  6.3 2.2
# The sorcerer's granted meteor spam over the void tier — impacts rain from 3 s on.
clip g_meteor     2.9 2.5
clip g_tinkerer2  19.4 3.0
clip g_engineer2  10.0 3.0
clip g_paladin    16.6 3.0
clip gw_flask     4.0 2.2
clip g_rogue      2.0 3.0
clip g_wanderer   12.5 3.0
clip g_descent    7.0 3.0
# The boss reel, ascending floors (Butcher as the armor-only DUEL; Grim Reaper unreachable).
clip g_butcher    5.4 2.0
clip g_ygara      13.2 2.0
clip g_malachar   8.3 2.0
clip g_azhar      13.8 2.0
clip g_diabro     6.4 2.0
clip g_nyx        28.2 2.0
clip g_chakram    4.0 3.5
clip g_couch      16.0 2.5
clip g_death      8.5 1.5
pngcard "$ST/card_out_now.png" 2.5 out
finish "$TAKES/trailer_gameplay_v9.mp4"
