# TEMPLATE — copy into tools/gen_mesh.py and rename gen_player_<class>.
# Modeled on gen_humanoid (tools/gen_mesh.py ~line 322). Voxel-based: build a
# set of filled (gx, gy, gz) grid cells, then call add_voxel_model.
#
# CONVENTIONS:
#   - Origin at FEET: the lowest body voxel sits at gy=0 (the renderer scales the model so
#     the bounding box bottom rests on the floor — see engine_render_world.cpp's
#     targetH = 1.8f / meshH scale path).
#   - One voxel column at (gx, gy) maps to ONE skin pixel (see skin_generator.py).
#   - vs = height / N picks the voxel size so the model is N voxels tall.
#     Players default to height=1.8 (matches the renderer's targetH); 16 voxels tall keeps
#     the look consistent with NPCs, but bump to 18-20 for bulky armor or drop to 14 for
#     slim/short silhouettes.
#   - RECORD the grid extents you use (min/max gx and gy) — skin_player_<class>() must use
#     the SAME w = max_gx-min_gx+1 and h = max_gy-min_gy+1.
#   - DISTINCT SILHOUETTE: the simplest way to make a class read at a glance is shape, not
#     color. Pauldrons, hoods, capes, weapon-back-mounts, height differences, etc. all show
#     up before the pixel palette does.

def gen_player_<class>(height=1.8):
    """<one-line class visual brief>. Origin at feet (Y=0)."""
    mb = MeshBuilder()
    vs = height / 16.0          # 16 voxels tall; adjust divisor for chunkier/finer
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Body — TODO: shape your class here ------------------------------------
    # Example placeholder bipedal blob. Replace with the class's iconic silhouette
    # (e.g. broad shoulders for Warrior, hooded robe for Sorcerer, duster coat for
    # Marksman, exosuit for Combat Engineer).
    fill_box(-2, 11, -2, 5, 4, 4)   # head  (gx -2..2, gy 11..14)
    fill_box(-2,  5, -1, 5, 6, 3)   # torso (gx -2..2, gy 5..10)
    fill_box(-2,  0,  0, 1, 5, 1)   # left leg
    fill_box( 2,  0,  0, 1, 5, 1)   # right leg
    fill_box(-3,  6,  0, 1, 4, 1)   # left arm
    fill_box( 3,  6,  0, 1, 4, 1)   # right arm

    # Carve a face-only feature by discarding only the FRONT layer (gz = min z), so the
    # voxel behind it still gets a skin color (see gen_humanoid eye sockets):
    #   filled.discard((-1, 13, -2))   # left "eye" front face

    # Grid extents for THIS model (keep skin_player_<class>() in sync):
    #   gx in [-3, 3]  -> w = 7
    #   gy in [ 0, 14] -> h = 15

    ox = -0.5 * vs              # center the model on X/Z (origin between feet)
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


# Then register in MESH_TYPES (tools/gen_mesh.py ~line 3328):
#   "player_<class>": {"func": gen_player_<class>, "desc": "<desc>. Params: --height",
#                       "default_file": "player_<class>.obj"},
#
# And add to build_meshes() in tools/build_assets.py (under "# Player class meshes"):
#   ["--type", "player_<class>", "--out", os.path.join(mesh_dir, "player_<class>.obj")],
