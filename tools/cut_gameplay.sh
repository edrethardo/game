#!/usr/bin/env bash
# cut_gameplay.sh — the GAMEPLAY trailer's edit list (WB-273, epic WB-267). v6.
#
# v6 is the full NATIVE-1080p re-shoot (user: "Nimm ihn komplett neu in 1080p auf") with a
# spectacle-first structure: every beat is dense combat — frozen orb into a pack, the molten
# Hellforge sea, the two-story brawl, swarm/tesla, the Descent's vertical drops, the superboss,
# the chakram storm — and the quiet menu beat from v5 is CUT (a 2 s inventory pan is the one
# thing in the old cut that was not a spectacle). Dungeon-only stays the rule.
source "$(dirname "$0")/cut_common.sh"
ST="$(dirname "$0")/../store/trailer"

pngcard "$ST/card_actual_gameplay.png" 1.6 in
# Opener: the sorcerer's frozen-orb window — trim rides the peak-blue frames
# (tools/scan_take.py --blue), not sampling luck.
clip g_orb        4.2 4.0
clip g_lava       8.5 3.5
clip g_vhall      21.3 3.5
pngcard "$ST/card_9_classes.png" 1.3
clip g_tinkerer   10.8 3.5
clip g_engineer   9.6 3.5
clip g_descent    7.0 3.5
# Three milestone bosses, not the Dungeon Engine (user call): the teleporter Nyx, the
# namesake DiaBRO, and the Broodqueen's add-swarm — each on Hell so the fight lasts.
pngcard "$ST/card_11_bosses.png" 1.3
clip g_nyx        28.2 3.0
clip g_diabro     6.4 3.0
clip g_ygara      13.2 3.0
clip g_chakram    4.0 4.5
pngcard "$ST/card_couch.png" 1.3
clip g_couch      16.0 3.5
clip g_death      8.5 1.5
pngcard "$ST/card_out_now.png" 2.5 out
finish "$TAKES/trailer_gameplay_v6.mp4"
