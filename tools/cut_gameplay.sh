#!/usr/bin/env bash
# cut_gameplay.sh — the GAMEPLAY trailer's edit list (WB-273, epic WB-267). v3.
#
# v2 made it dungeon-only with the real superboss; v3 swaps the drawtext cards for the old title
# art's gold-wordmark cards (store/trailer) — same reasoning as the cinematic's v3.
source "$(dirname "$0")/cut_common.sh"
ST="$(dirname "$0")/../store/trailer"

pngcard "$ST/card_actual_gameplay.png" 1.6 in
# The opener is the SORCERER's frozen-orb window (user: the cut was near-all melee, and "Frozen
# Orb sieht geil aus") — the trim sits on the take's peak-blue frames, found by a colour scan
# over the frames rather than sampling luck.
clip sorcerer_d       10.6 4.0
clip vhall_run         3.0 4.0
pngcard "$ST/card_9_classes.png" 1.3
clip class_tinkerer_d  5.0 3.5
clip class_engineer_d  3.0 3.5
clip dungeon_run       4.0 3.5
pngcard "$ST/card_dungeon_engine.png" 1.3
clip source_boss       3.0 6.0
# No INFINITE CHAKRAMS card (same call as the cinematic): the throw/flight/hits ARE the pitch.
clip chakram_player    2.0 4.5
clip menu_beat         1.0 2.0
pngcard "$ST/card_couch.png" 1.3
clip couch_coop       10.5 3.5
clip death_dungeon     4.25 1.5
pngcard "$ST/card_out_now.png" 2.5 out
finish "$TAKES/trailer_gameplay_v5.mp4"
