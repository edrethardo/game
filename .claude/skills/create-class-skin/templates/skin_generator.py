# TEMPLATE — copy into tools/gen_skin.py and rename skin_player_<class>.
# Modeled on skin_skeleton (tools/gen_skin.py ~line 45). Returns (w, h, pixel_map)
# where pixel_map is {(px, py): (r, g, b, a)} — a tiny per-voxel-column palette.
#
# GRID ALIGNMENT (critical): the grid MUST match gen_player_<class>() in gen_mesh.py.
#   w = max_gx - min_gx + 1
#   h = max_gy - min_gy + 1
#   a mesh voxel at (gx, gy) reads skin pixel (px, py) = (gx - min_gx, gy - min_gy)
# So if the mesh uses gx in [-3,3], gy in [0,14], then w=7, h=15, and the column
# at mesh gx=-3 is skin px=0; mesh gy=0 is skin py=0.
#
# NOTE: a pixel color shows on ALL faces of every voxel in that column — keep
# "eye"/accent colors muted so they read on the sides too. Use a "face-only" carve
# in gen_player_<class> (discard the front-facing voxel) if a feature must be face-only.
#
# COLOR IDENTITY: a class is usually recognized first by shape, second by its 2-3
# dominant colors. Pick a base, a trim/accent, and a metal/cloth secondary, then keep
# everything else near-base. Boots/pants/gloves are good places to repeat the accent.

def skin_player_<class>():
    """Grid: x=[<min_gx>,<max_gx>] (w=<w>), y=[<min_gy>,<max_gy>] (h=<h>). <class brief>."""
    w, h = 7, 15            # TODO: match the mesh grid extents
    p = {}

    base   = (120, 100, 90, 255)        # TODO: body/clothes base color
    accent = (180, 150, 60, 255)        # TODO: trim / sash / emblem color
    metal  = (110, 110, 120, 255)       # TODO: secondary (boots, gloves, weapon)

    # Default everything to base, then override with bands/accents:
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # --- Example accents (px/py are skin coords = mesh gx/gy minus the mins) ---
    # Head band (chest emblem, hood trim, etc):
    #   for px in range(1, w - 1): p[(px, h - 2)] = accent
    # Eyes (muted — show on the sides too):
    #   p[(2, h - 2)] = (40, 20, 15, 255)
    #   p[(4, h - 2)] = (40, 20, 15, 255)
    # Boots / gloves (use the metal color along the limb columns):
    #   for py in range(0, 3):  p[(0, py)] = metal; p[(w - 1, py)] = metal

    return w, h, p


# Then register in SKIN_TYPES (tools/gen_skin.py ~line 3812):
#   "player_<class>": ("player_<class>_skin_42.png", skin_player_<class>),
#
# And add to build_skins() in tools/build_assets.py (under "# Player class skins"):
#   ("player_<class>", "player_<class>_skin_42.png"),
