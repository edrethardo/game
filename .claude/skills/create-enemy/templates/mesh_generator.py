# TEMPLATE — copy into tools/gen_mesh.py and rename gen_<name>.
# Modeled on gen_humanoid (tools/gen_mesh.py ~line 322). Voxel-based: build a
# set of filled (gx, gy, gz) grid cells, then call add_voxel_model.
#
# CONVENTIONS:
#   - Origin at FEET: the lowest body voxel sits at gy=0 (so it stands on the floor).
#   - One voxel column at (gx, gy) maps to ONE skin pixel (see skin_generator.py).
#   - vs = height / N  picks the voxel size so the model is N voxels tall.
#   - RECORD the grid extents you use (min/max gx and gy) — skin_<name>() must use
#     the SAME w = max_gx-min_gx+1 and h = max_gy-min_gy+1.

def gen_<name>(height=1.8):
    """<one-line description>. Origin at feet (Y=0)."""
    mb = MeshBuilder()
    vs = height / 16.0          # 16 voxels tall; adjust divisor for chunkier/finer
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Body — TODO: shape your creature here ---------------------------------
    # Example placeholder bipedal blob. Replace with real geometry.
    fill_box(-2, 11, -2, 5, 4, 4)   # head  (gx -2..2, gy 11..14)
    fill_box(-2,  5, -1, 5, 6, 3)   # torso (gx -2..2, gy 5..10)
    fill_box(-2,  0,  0, 1, 5, 1)   # left leg
    fill_box( 2,  0,  0, 1, 5, 1)   # right leg
    fill_box(-3,  6,  0, 1, 4, 1)   # left arm
    fill_box( 3,  6,  0, 1, 4, 1)   # right arm

    # Carve a feature by discarding only the FRONT layer (gz = min z), so the
    # voxel behind it still gets a skin color (see gen_humanoid eye sockets):
    #   filled.discard((-1, 13, -2))   # left "eye" front face

    # Grid extents for THIS model (keep skin_<name>() in sync):
    #   gx in [-3, 3]  -> w = 7
    #   gy in [ 0, 14] -> h = 15

    ox = -0.5 * vs              # center the model on X/Z (origin between feet)
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


# Then register in MESH_TYPES (tools/gen_mesh.py ~line 3328):
#   "<name>": {"func": gen_<name>, "desc": "<desc>. Params: --height", "default_file": "<name>.obj"},
#
# And add to build_meshes() in tools/build_assets.py:
#   ["--type", "<name>", "--height", "1.8", "--out", os.path.join(mesh_dir, "<name>.obj")],
