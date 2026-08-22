#!/usr/bin/env python3
"""Generate the trailers' MID-CARDS in the old title sequence's own style (user call, 2026-08-22:
"wir haben schon die Texteinblendungen — die waren cooler").

Reuses gen_trailer_frames' scene master + draw_word, so every card is the gold/red wordmark look
over the logo's dungeon-stairs art — one visual language from the first frame to the last, instead
of the plain drawtext-on-black cards of trailer v1/v2.

Also composes the END card: the full logo frame with the wishlist line beneath it.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
import gen_trailer_frames as gtf

GOLD = (250, 205, 60)     # DUNGEON gold (matches draw_title)
RED  = (200, 40, 35)      # ENGINE red
SUB  = (200, 180, 120)    # CURSE OF THE subtitle tone

CARDS = [
    ("card_actual_gameplay.png", [("ACTUAL GAMEPLAY", SUB)],            0.62),
    ("card_9_classes.png",       [("9 CLASSES", GOLD)],                 0.52),
    ("card_dungeon_engine.png",  [("THE", SUB), ("DUNGEON ENGINE", RED)], 0.62),
    # v6: the boss beat shows three MILESTONE bosses (user: no Dungeon Engine fight in the
    # trailer) — the count card mirrors "9 CLASSES" and 11 is the real bosses.json roster.
    ("card_11_bosses.png",       [("11 BOSSES", RED)],                  0.55),
    ("card_infinite.png",        [("INFINITE", SUB), ("CHAKRAMS", GOLD)], 0.55),
    ("card_couch.png",           [("COUCH CO-OP", GOLD)],               0.58),
]

def main():
    outdir = os.path.join(gtf.ROOT_DIR, "store", "trailer")
    master = gtf.render_scene_master(3840)
    # A mid-zoom camera window: recognisably the SAME art as the title sequence without being
    # identical to any of its five frames.
    base = gtf.camera_crop(master, 0.72)
    for name, lines, wf in CARDS:
        img = gtf.draw_word(base.copy(), lines, width_frac=wf, cy_frac=0.5)
        img.convert("RGB").save(os.path.join(outdir, name))
        print("wrote", name)
    # End card: the real full-logo frame + the call to action. OUT NOW, not "wishlist" — the game
    # is already in Early Access (user, 2026-08-22), and a trailer asking people to wishlist a
    # game they can buy is the one mistake a store page cannot afford.
    logo = Image.open(os.path.join(outdir, "frame_5_logo.png")).convert("RGBA")
    end = gtf.draw_word(logo, [("OUT NOW IN EARLY ACCESS", GOLD)], width_frac=0.5, cy_frac=0.88)
    end.convert("RGB").save(os.path.join(outdir, "card_out_now.png"))
    print("wrote card_out_now.png")

if __name__ == "__main__":
    main()
