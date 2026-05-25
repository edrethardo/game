# TEMPLATE — copy into tools/gen_skin.py and rename skin_<name>.
# Modeled on skin_skeleton (tools/gen_skin.py ~line 45). Returns (w, h, pixel_map)
# where pixel_map is {(px, py): (r, g, b, a)} — a tiny per-voxel-column palette.
#
# GRID ALIGNMENT (critical): the grid MUST match gen_<name>() in gen_mesh.py.
#   w = max_gx - min_gx + 1
#   h = max_gy - min_gy + 1
#   a mesh voxel at (gx, gy) reads skin pixel (px, py) = (gx - min_gx, gy - min_gy)
# So if the mesh uses gx in [-3,3], gy in [0,14], then w=7, h=15, and the column
# at mesh gx=-3 is skin px=0; mesh gy=0 is skin py=0.
#
# NOTE: a pixel color shows on ALL faces of every voxel in that column — keep
# "eye"/accent colors muted so they read on the sides too.

def skin_<name>():
    """Grid: x=[<min_gx>,<max_gx>] (w=<w>), y=[<min_gy>,<max_gy>] (h=<h>)."""
    w, h = 7, 15            # TODO: match the mesh grid extents
    p = {}

    base = (150, 120, 90, 255)          # TODO: body base color
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # --- Accents (examples; px/py are skin coords = mesh gx/gy minus the mins) ---
    # Head band lighter:
    #   for px in range(1, w - 1): p[(px, h - 2)] = (180, 150, 120, 255)
    # Eyes (muted):
    #   p[(2, h - 2)] = (40, 20, 15, 255)
    #   p[(4, h - 2)] = (40, 20, 15, 255)
    # Limb shading:
    #   for py in range(2, 8): p[(0, py)] = (120, 95, 70, 255); p[(w - 1, py)] = (120, 95, 70, 255)

    return w, h, p


# Then register in SKIN_TYPES (tools/gen_skin.py ~line 3812):
#   "<name>": ("<name>_skin_42.png", skin_<name>),
#
# And add to build_skins() in tools/build_assets.py:
#   ("<name>", "<name>_skin_42.png"),
