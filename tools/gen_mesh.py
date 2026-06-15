#!/usr/bin/env python3
"""Generate low-poly 3D meshes as OBJ files for the game engine.

Usage examples:
    python3 tools/gen_mesh.py --type humanoid --height 1.8 --out assets/meshes/skeleton.obj
    python3 tools/gen_mesh.py --type spider --radius 0.6 --out assets/meshes/spider.obj
    python3 tools/gen_mesh.py --type bat --wingspan 1.0 --out assets/meshes/bat.obj
    python3 tools/gen_mesh.py --type pillar --radius 0.3 --height 3.0 --sides 8 --out assets/meshes/pillar.obj
    python3 tools/gen_mesh.py --type chest --width 0.6 --out assets/meshes/chest.obj
    python3 tools/gen_mesh.py --list-types
"""

import argparse
import math
import os
import sys

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _normalize(v):
    length = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    if length < 1e-12:
        return (0.0, 1.0, 0.0)
    return (v[0] / length, v[1] / length, v[2] / length)


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


# ---------------------------------------------------------------------------
# Mesh accumulator
# ---------------------------------------------------------------------------

class MeshBuilder:
    """Collects verts, normals, uvs, and triangle faces."""

    def __init__(self):
        self.verts = []   # list of (x, y, z)
        self.normals = []  # list of (nx, ny, nz)
        self.uvs = []      # list of (u, v)
        # Each face is ((vi,ti,ni), (vi,ti,ni), (vi,ti,ni), material_name_or_None)
        self.faces = []
        self._current_material = None  # active usemtl name, None = no material groups

    # indices are 0-based internally; converted to 1-based on write.

    def add_vert(self, x, y, z):
        self.verts.append((x, y, z))
        return len(self.verts) - 1

    def add_normal(self, nx, ny, nz):
        self.normals.append((nx, ny, nz))
        return len(self.normals) - 1

    def add_uv(self, u, v):
        self.uvs.append((u, v))
        return len(self.uvs) - 1

    def set_material(self, name):
        """Set the current material name for subsequent faces (emits usemtl in write_obj)."""
        self._current_material = name

    def add_tri(self, v0, v1, v2, t0, t1, t2, n0, n1, n2):
        self.faces.append(((v0, t0, n0), (v1, t1, n1), (v2, t2, n2), self._current_material))


# ---------------------------------------------------------------------------
# add_box — axis-aligned box with per-face normals (12 triangles)
# ---------------------------------------------------------------------------

def add_box(mb, center, half_extents):
    """Add an axis-aligned box to the MeshBuilder.

    center: (cx, cy, cz)
    half_extents: (hx, hy, hz)
    Generates 8 corners, 6 face normals, simple 0-1 UVs per face, 12 tris.
    """
    cx, cy, cz = center
    hx, hy, hz = half_extents

    # 8 corners
    corners = [
        (cx - hx, cy - hy, cz - hz),  # 0: left  bottom back
        (cx + hx, cy - hy, cz - hz),  # 1: right bottom back
        (cx + hx, cy + hy, cz - hz),  # 2: right top    back
        (cx - hx, cy + hy, cz - hz),  # 3: left  top    back
        (cx - hx, cy - hy, cz + hz),  # 4: left  bottom front
        (cx + hx, cy - hy, cz + hz),  # 5: right bottom front
        (cx + hx, cy + hy, cz + hz),  # 6: right top    front
        (cx - hx, cy + hy, cz + hz),  # 7: left  top    front
    ]

    vi = [mb.add_vert(*c) for c in corners]

    # 6 face normals
    face_defs = [
        # (normal, quad corners in CCW order when viewed from outside)
        ((0, 0, -1), (1, 0, 3, 2)),   # back  (-Z)
        ((0, 0,  1), (4, 5, 6, 7)),   # front (+Z)
        ((-1, 0, 0), (0, 4, 7, 3)),   # left  (-X)
        (( 1, 0, 0), (5, 1, 2, 6)),   # right (+X)
        ((0, -1, 0), (0, 1, 5, 4)),   # bottom(-Y)
        ((0,  1, 0), (3, 7, 6, 2)),   # top   (+Y)
    ]

    # Shared UVs for each face quad: 4 corners
    uv0 = mb.add_uv(0.0, 0.0)
    uv1 = mb.add_uv(1.0, 0.0)
    uv2 = mb.add_uv(1.0, 1.0)
    uv3 = mb.add_uv(0.0, 1.0)

    for normal, (a, b, c, d) in face_defs:
        ni = mb.add_normal(*normal)
        # two triangles per quad
        mb.add_tri(vi[a], vi[b], vi[c], uv0, uv1, uv2, ni, ni, ni)
        mb.add_tri(vi[a], vi[c], vi[d], uv0, uv2, uv3, ni, ni, ni)


# ---------------------------------------------------------------------------
# add_cylinder — N-sided cylinder with caps
# ---------------------------------------------------------------------------

def add_cylinder(mb, base_center, radius, height, sides):
    """N-sided cylinder with top and bottom caps, cylindrical UVs on walls."""
    cx, cy, cz = base_center

    bottom_vis = []
    top_vis = []
    for i in range(sides):
        angle = 2.0 * math.pi * i / sides
        x = cx + radius * math.cos(angle)
        z = cz + radius * math.sin(angle)
        bottom_vis.append(mb.add_vert(x, cy, z))
        top_vis.append(mb.add_vert(x, cy + height, z))

    bot_center = mb.add_vert(cx, cy, cz)
    top_center = mb.add_vert(cx, cy + height, cz)

    bottom_n = mb.add_normal(0, -1, 0)
    top_n = mb.add_normal(0, 1, 0)

    side_normals = []
    for i in range(sides):
        angle = 2.0 * math.pi * (i + 0.5) / sides
        side_normals.append(mb.add_normal(math.cos(angle), 0, math.sin(angle)))

    uv00 = mb.add_uv(0.0, 0.0)
    uv10 = mb.add_uv(1.0, 0.0)
    uv11 = mb.add_uv(1.0, 1.0)
    uv01 = mb.add_uv(0.0, 1.0)
    uv_center = mb.add_uv(0.5, 0.5)

    for i in range(sides):
        j = (i + 1) % sides
        sn = side_normals[i]
        # CCW winding when viewed from outside
        mb.add_tri(bottom_vis[i], top_vis[j], bottom_vis[j],
                   uv00, uv11, uv10, sn, sn, sn)
        mb.add_tri(bottom_vis[i], top_vis[i], top_vis[j],
                   uv00, uv01, uv11, sn, sn, sn)
        mb.add_tri(bot_center, bottom_vis[i], bottom_vis[j],
                   uv_center, uv00, uv10, bottom_n, bottom_n, bottom_n)
        mb.add_tri(top_center, top_vis[j], top_vis[i],
                   uv_center, uv10, uv00, top_n, top_n, top_n)


# ---------------------------------------------------------------------------
# Voxel model builder — Barony-style chunky cube models
# ---------------------------------------------------------------------------

def add_voxel_model(mb, filled, voxel_size, offset=(0, 0, 0),
                    uv_overrides=None):
    """Build optimized mesh from a set of filled voxel positions.

    filled: set of (gx, gy, gz) integer grid positions that are filled
    voxel_size: world-space size of each cube
    offset: world-space offset applied to all voxels
    uv_overrides: optional dict {(gx,gy,gz): (alt_gx, alt_gy)} — remap
        specific voxels to a different skin pixel, e.g. to prevent eye
        color bleeding to back-of-head voxels in the same (gx,gy) column.

    Only emits faces at filled/empty boundaries (internal faces culled).

    UVs map each voxel to a pixel based on (gx, gy). All faces of the same
    voxel share the same color. Simple, stable, agent-friendly.
    """
    ox, oy, oz = offset
    hs = voxel_size * 0.5

    if not filled:
        return
    min_gx = min(p[0] for p in filled)
    max_gx = max(p[0] for p in filled)
    min_gy = min(p[1] for p in filled)
    max_gy = max(p[1] for p in filled)
    grid_w = max_gx - min_gx + 1
    grid_h = max_gy - min_gy + 1
    tex_w = max(grid_w, 1)
    tex_h = max(grid_h, 1)

    # Each entry: (neighbour offset, face normal, quad corners in CCW order from outside)
    face_defs = [
        ((0, 1, 0),  (0, 1, 0),  [(-1,1,-1), (-1,1,1), (1,1,1), (1,1,-1)]),
        ((0,-1, 0),  (0,-1, 0),  [(-1,-1,1), (-1,-1,-1), (1,-1,-1), (1,-1,1)]),
        ((1, 0, 0),  (1, 0, 0),  [(1,-1,-1), (1,1,-1), (1,1,1), (1,-1,1)]),
        ((-1,0, 0),  (-1,0, 0),  [(-1,-1,1), (-1,1,1), (-1,1,-1), (-1,-1,-1)]),
        ((0, 0, 1),  (0, 0, 1),  [(-1,-1,1), (1,-1,1), (1,1,1), (-1,1,1)]),
        ((0, 0,-1),  (0, 0,-1),  [(1,-1,-1), (-1,-1,-1), (-1,1,-1), (1,1,-1)]),
    ]

    normal_cache = {}
    for _, norm, _ in face_defs:
        if norm not in normal_cache:
            normal_cache[norm] = mb.add_normal(*norm)

    for (gx, gy, gz) in filled:
        cx = ox + gx * voxel_size + hs
        cy = oy + gy * voxel_size + hs
        cz = oz + gz * voxel_size + hs

        # UV defaults to (gx, gy) pixel; per-voxel override remaps to a
        # different pixel (e.g. skin color instead of eye color at back of head)
        ugx, ugy = gx, gy
        if uv_overrides and (gx, gy, gz) in uv_overrides:
            ugx, ugy = uv_overrides[(gx, gy, gz)]
        u_center = (ugx - min_gx + 0.5) / tex_w
        v_center = (ugy - min_gy + 0.5) / tex_h
        eps = 0.01 / tex_w
        uv0 = mb.add_uv(u_center - eps, v_center - eps)
        uv1 = mb.add_uv(u_center + eps, v_center - eps)
        uv2 = mb.add_uv(u_center + eps, v_center + eps)
        uv3 = mb.add_uv(u_center - eps, v_center + eps)

        for (dx, dy, dz), norm, corners in face_defs:
            if (gx + dx, gy + dy, gz + dz) in filled:
                continue

            ni = normal_cache[norm]
            vis = []
            for (sx, sy, sz) in corners:
                vis.append(mb.add_vert(cx + sx * hs, cy + sy * hs, cz + sz * hs))

            mb.add_tri(vis[0], vis[1], vis[2], uv0, uv1, uv2, ni, ni, ni)
            mb.add_tri(vis[0], vis[2], vis[3], uv0, uv2, uv3, ni, ni, ni)

    # Store grid info for skin texture generation
    mb.grid_min_x = min_gx
    mb.grid_min_y = min_gy
    mb.grid_w = grid_w
    mb.grid_h = grid_h
    mb.filled = filled


# ---------------------------------------------------------------------------
# write_obj
# ---------------------------------------------------------------------------

def write_obj(path, mb):
    """Write a MeshBuilder to an OBJ file.

    Emits usemtl directives when faces carry material names (set via set_material()).
    Faces without a material name are written without a usemtl header.
    """
    dirpath = os.path.dirname(path)
    if dirpath:
        os.makedirs(dirpath, exist_ok=True)

    with open(path, "w") as f:
        f.write("# Generated by gen_mesh.py\n")
        f.write(f"# Vertices: {len(mb.verts)}  Normals: {len(mb.normals)}  "
                f"UVs: {len(mb.uvs)}  Faces: {len(mb.faces)}\n\n")

        for x, y, z in mb.verts:
            f.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        f.write("\n")

        for nx, ny, nz in mb.normals:
            f.write(f"vn {nx:.6f} {ny:.6f} {nz:.6f}\n")
        f.write("\n")

        for u, v in mb.uvs:
            f.write(f"vt {u:.6f} {v:.6f}\n")
        f.write("\n")

        active_material = object()  # sentinel that won't match any string or None
        for tri in mb.faces:
            # Support both old 3-element tuples and new 4-element (with material name)
            if len(tri) == 4:
                v0, v1, v2, mat_name = tri
            else:
                v0, v1, v2 = tri
                mat_name = None

            # Emit usemtl when material changes between faces
            if mat_name != active_material:
                if mat_name is not None:
                    f.write(f"usemtl {mat_name}\n")
                active_material = mat_name

            parts = []
            for vi, ti, ni in (v0, v1, v2):
                # OBJ is 1-based
                parts.append(f"{vi+1}/{ti+1}/{ni+1}")
            f.write(f"f {' '.join(parts)}\n")


# ---------------------------------------------------------------------------
# Mesh generators
# ---------------------------------------------------------------------------

def gen_humanoid(height=1.8):
    """Barony-style chunky voxel skeleton. Origin at feet (Y=0).

    Big skull head with eye sockets and jaw, visible rib cage torso,
    bony arms and legs. Cartoony proportions — oversized head.
    """
    mb = MeshBuilder()
    vs = height / 16.0  # voxel size — 16 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Skull (big, cartoony) ---
    # Main skull dome — 5x4x4
    fill_box(-2, 12, -2, 5, 4, 4)
    # Jaw — 3x1x3, slightly narrower
    fill_box(-1, 11, -2, 3, 1, 3)
    # Eye sockets — remove only front layer so the back is colored by skin texture
    filled.discard((-1, 14, -2))  # left eye front
    filled.discard((1, 14, -2))   # right eye front
    # Keep (-1,14,-1) and (1,14,-1) — these are the visible eye voxels
    # Nose — remove only front
    filled.discard((0, 13, -2))
    # Mouth — remove front layer only, back voxels stay for teeth coloring
    filled.discard((0, 11, -2))   # center mouth gap

    # Neck — 1x1x1
    fill_box(0, 10, 0, 1, 1, 1)

    # --- Rib cage (open at front) ---
    # Spine column — 1 voxel wide, 5 tall
    fill_box(0, 5, 1, 1, 5, 1)
    # Rib pairs — 3 rows, leaving front open
    for ry in [6, 7, 9]:
        fill_box(-2, ry, 0, 2, 1, 2)  # left ribs
        fill_box(1, ry, 0, 2, 1, 2)   # right ribs

    # Shoulder bones
    fill_box(-3, 9, 0, 1, 1, 1)
    fill_box(3, 9, 0, 1, 1, 1)

    # --- Arms (bony, 1 voxel thick) ---
    # Upper arms
    fill_box(-3, 7, 0, 1, 2, 1)
    fill_box(3, 7, 0, 1, 2, 1)
    # Lower arms
    fill_box(-3, 4, 0, 1, 3, 1)
    fill_box(3, 4, 0, 1, 3, 1)
    # Hands — small blocks
    fill_box(-3, 3, -1, 1, 1, 2)
    fill_box(3, 3, -1, 1, 1, 2)

    # --- Pelvis ---
    fill_box(-1, 4, 0, 3, 1, 1)

    # --- Legs (bony) ---
    # Thigh bones
    fill_box(-2, 2, 0, 1, 2, 1)
    fill_box(1, 2, 0, 1, 2, 1)
    # Shin bones
    fill_box(-2, 0, 0, 1, 2, 1)
    fill_box(1, 0, 0, 1, 2, 1)
    # Feet — flat 2x1 blocks
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(1, 0, -1, 1, 1, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    # UV overrides: prevent eye color bleeding to back-of-head voxels.
    # gz=-2 is removed (eye socket hole), gz=-1 is the visible eye — keep it.
    # Only remap gz=0 and gz=1 (back of head) to non-eye pixel.
    uv_fix = {}
    for gz in range(0, 2):
        uv_fix[(-1, 14, gz)] = (0, 14)
        uv_fix[( 1, 14, gz)] = (0, 14)
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)

    return mb


def gen_spider(radius=0.6):
    """Barony-style chunky voxel spider. Fat body, fangs, jointed legs.
    Legs are part of the body mesh (static). Only mandibles are animated via limb system.
    Origin at center-bottom."""
    mb = MeshBuilder()
    vs = radius / 5.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Fat abdomen
    fill_box(-3, 1, 1, 6, 4, 6)
    for corner in [(-3,4,1),(-3,4,6),(2,4,1),(2,4,6),
                   (-3,1,1),(-3,1,6),(2,1,1),(2,1,6)]:
        filled.discard(corner)
    fill_box(-2, 5, 2, 4, 1, 4)
    # Thorax
    fill_box(-2, 1, -3, 4, 3, 4)
    # Head
    fill_box(-1, 1, -6, 3, 3, 3)
    filled.add((-1, 4, -5)); filled.add((1, 4, -5))
    filled.add((-1, 6, -5)); filled.add((1, 6, -5))
    # Fangs
    filled.add((-1, 0, -7)); filled.add((1, 0, -7))
    filled.add((-1, 0, -6)); filled.add((1, 0, -6))
    # Pedipalps
    filled.add((-1, 1, -7)); filled.add((1, 1, -7))
    # 8 legs with knee joints (static, part of body mesh)
    for sz in [-2, -1, 0, 1]:
        filled.add((-3, 2, sz)); filled.add((-4, 3, sz)); filled.add((-5, 4, sz))
        filled.add((-6, 3, sz)); filled.add((-6, 2, sz)); filled.add((-6, 1, sz))
        filled.add((-7, 0, sz))
        filled.add((3, 2, sz)); filled.add((4, 3, sz)); filled.add((5, 4, sz))
        filled.add((6, 3, sz)); filled.add((6, 2, sz)); filled.add((6, 1, sz))
        filled.add((7, 0, sz))
    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_spider_leg_pair(radius=0.6):
    """One pair of spider legs matching the original spider style exactly.
    Left side only — mirrored at render time. 2 legs at different Z.
    Shape: short outward climb to knee, then sharp vertical drop to foot.
    Origin at attachment point (gx=0 = body edge)."""
    mb = MeshBuilder()
    vs = radius / 5.0
    filled = set()
    leg_voxels = []

    # Original leg pattern (shifted so attachment is at gx=0):
    #   (0,2) attachment → (-1,3) out+up → (-2,4) KNEE → (-3,3) drop
    #   → (-3,2) → (-3,1) → (-4,0) FOOT
    def add_leg(sz):
        for v in [(0,2,sz), (-1,3,sz), (-2,4,sz),
                  (-3,3,sz), (-3,2,sz), (-3,1,sz), (-4,0,sz)]:
            filled.add(v); leg_voxels.append(v)

    add_leg(-1)  # front leg of pair
    add_leg(1)   # rear leg of pair

    # Remap all voxels to safe dark leg color (avoid eye pixel sampling)
    uv_overrides = {v: (0, 2) for v in leg_voxels}

    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_overrides)
    return mb


def gen_bat(wingspan=1.0):
    """Barony-style chunky voxel bat. Body + head + ears only.

    Wings and claws are handled by the LimbSystem at runtime, so they
    are NOT included in this body mesh to avoid doubling up.
    Origin at center-bottom.
    """
    mb = MeshBuilder()
    vs = wingspan / 16.0  # voxel size — 16 voxels wide
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Body — 3x4x3, chunky fur ball ---
    fill_box(-1, 2, -1, 3, 4, 3)

    # --- Head — 3x3x3, big cartoony head ---
    fill_box(-1, 6, -1, 3, 3, 3)
    # Snout — 1 voxel protruding from front
    filled.add((0, 6, -2))
    filled.add((0, 7, -2))
    # Eyes stay filled — colored bright by the skin texture instead of hollow
    # (hollow sockets are invisible in dark dungeons)
    # Mouth stays filled too — skin texture handles the color

    # --- Big pointy ears ---
    filled.add((-1, 9, 0))
    filled.add((-1, 10, 0))
    filled.add((-1, 11, 0))  # tall ear
    filled.add((1, 9, 0))
    filled.add((1, 10, 0))
    filled.add((1, 11, 0))

    # --- Small shoulder stubs where wings attach (visual anchor) ---
    filled.add((-2, 4, 0))
    filled.add((2, 4, 0))

    # NOTE: Wings and claws are NOT generated here — they are rendered
    # as separate animated limbs by LimbSystem at runtime.

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))

    return mb


def gen_pillar(radius=0.3, height=3.0, sides=8):
    """N-sided cylinder pillar. Origin at base center."""
    mb = MeshBuilder()
    add_cylinder(mb, base_center=(0, 0, 0), radius=radius, height=height, sides=sides)
    return mb


def gen_chest(width=0.6):
    """Simple chest: bottom box + slightly raised lid box."""
    mb = MeshBuilder()

    depth = width * 0.67
    total_h = width * 0.67

    bottom_h = total_h * 0.6
    lid_h = total_h * 0.35
    gap = total_h * 0.02  # small gap between body and lid

    # Bottom box
    add_box(mb,
            center=(0, bottom_h * 0.5, 0),
            half_extents=(width * 0.5, bottom_h * 0.5, depth * 0.5))

    # Lid — slightly wider at the top, offset up with a small gap
    lid_y = bottom_h + gap + lid_h * 0.5
    add_box(mb,
            center=(0, lid_y, 0),
            half_extents=(width * 0.52, lid_h * 0.5, depth * 0.52))

    return mb


def gen_human(height=1.8):
    """Barony-style chunky human NPC. Origin at feet (Y=0).

    Broader build than skeleton, solid face with eyes/mouth indentations,
    thicker limbs. Meant for friendly NPC allies wearing armor.
    """
    mb = MeshBuilder()
    vs = height / 16.0  # voxel size — 16 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (round, solid) ---
    fill_box(-2, 13, -2, 5, 3, 4)  # main head block
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Hollow out eyes (front face only)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth indent
    filled.discard((0, 12, -2))

    # --- Neck ---
    fill_box(0, 11, 0, 1, 1, 1)

    # --- Torso (broad, solid — like a breastplate) ---
    fill_box(-2, 6, -1, 5, 5, 3)  # main chest
    fill_box(-3, 9, -1, 1, 2, 3)  # left shoulder pad
    fill_box(3, 9, -1, 1, 2, 3)   # right shoulder pad

    # Belt
    fill_box(-2, 5, -1, 5, 1, 3)

    # --- Arms (thick, 2 voxels) ---
    fill_box(-4, 7, 0, 1, 3, 1)   # left upper arm
    fill_box(4, 7, 0, 1, 3, 1)    # right upper arm
    fill_box(-4, 4, 0, 1, 3, 1)   # left lower arm
    fill_box(4, 4, 0, 1, 3, 1)    # right lower arm
    # Hands
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # --- Legs (thick) ---
    fill_box(-2, 2, -1, 2, 3, 2)  # left thigh
    fill_box(1, 2, -1, 2, 3, 2)   # right thigh
    fill_box(-2, 0, -1, 2, 2, 2)  # left shin
    fill_box(1, 0, -1, 2, 2, 2)   # right shin
    # Boots
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))

    return mb


def gen_cleric(height=1.8):
    """Male cleric NPC. Broad build, hooded head, robes, short blonde hair.

    Origin at feet (Y=0). Heavier torso for priestly armor look.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (round)
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eyes
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))
    # Short blonde hair (top of head, slightly wider)
    fill_box(-2, 16, -2, 5, 1, 4)
    fill_box(-2, 15, -2, 5, 1, 1)  # front fringe

    # Hood/collar
    fill_box(-2, 11, -1, 5, 2, 3)

    # Torso (priestly robes — wide, flowing)
    fill_box(-3, 5, -1, 7, 6, 3)
    # Shoulder guards
    fill_box(-4, 9, -1, 1, 2, 3)
    fill_box(4, 9, -1, 1, 2, 3)
    # Holy symbol on chest (small bump)
    filled.add((0, 8, -2))

    # Belt/sash
    fill_box(-3, 4, -1, 7, 1, 3)

    # Arms
    fill_box(-4, 7, 0, 1, 3, 1)
    fill_box(4, 7, 0, 1, 3, 1)
    fill_box(-4, 4, 0, 1, 3, 1)
    fill_box(4, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # Legs (hidden under robes — thicker)
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)
    # Boots
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    # Remap back-of-head voxels in eye columns to skin color so the eye
    # pixel doesn't bleed through to the back of the head.  gz=-1 is the
    # visible eyeball behind the socket — keep that as eye color.
    uv_fix = {}
    for gz in range(0, 2):      # gz=0 and gz=1
        uv_fix[(-1, 14, gz)] = (0, 14)   # center column = skin pixel
        uv_fix[(1, 14, gz)]  = (0, 14)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_archer(height=1.7):
    """Female archer NPC. Leaner build, ponytail, lighter armor.

    Origin at feet (Y=0). Slimmer than cleric, with a ponytail.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (slightly smaller, 5 wide to center eye sockets properly)
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eyes (green — just the sockets, tint does the color)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))

    # Fox-red/brown ponytail (extends behind head and down)
    fill_box(-2, 15, -2, 5, 2, 4)   # top hair (matches head width)
    fill_box(0, 14, 2, 1, 1, 1)     # ponytail start
    fill_box(0, 13, 2, 1, 1, 1)     # ponytail mid
    fill_box(0, 12, 2, 1, 1, 1)     # ponytail mid
    fill_box(0, 11, 2, 1, 1, 1)     # ponytail end
    fill_box(0, 10, 2, 1, 1, 1)     # ponytail tip

    # Neck
    fill_box(0, 11, 0, 1, 1, 1)

    # Upper torso — narrower waist, wider chest
    fill_box(-2, 8, -1, 4, 3, 3)   # upper chest
    fill_box(-2, 8, -2, 4, 2, 1)   # bust (front protrusion)
    # Light shoulder pads
    fill_box(-3, 9, 0, 1, 1, 1)
    fill_box(2, 9, 0, 1, 1, 1)

    # Narrow waist
    fill_box(-1, 6, -1, 3, 2, 3)

    # Wider hips
    fill_box(-2, 4, -1, 5, 2, 3)
    # Belt with quiver strap
    fill_box(-2, 5, -1, 5, 1, 3)
    # Quiver on back (tall thin box)
    fill_box(1, 6, 2, 1, 5, 1)

    # Arms (slimmer)
    fill_box(-3, 7, 0, 1, 3, 1)
    fill_box(2, 7, 0, 1, 3, 1)
    fill_box(-3, 4, 0, 1, 3, 1)
    fill_box(2, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-3, 3, -1, 1, 1, 2)
    fill_box(2, 3, -1, 1, 1, 2)

    # Legs (shapely — wider thigh, narrower shin)
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, 0, 1, 2, 1)
    fill_box(1, 0, 0, 1, 2, 1)
    # Light boots
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(1, 0, -1, 1, 1, 2)

    # Remap back-of-head voxels in eye columns to skin so eye color
    # doesn't bleed to the back.  Also remap ponytail voxels at face gy
    # levels to hair color so they don't pick up skin/face texture.
    uv_fix = {}
    for gz in range(0, 2):
        uv_fix[(-1, 14, gz)] = (0, 14)   # skin pixel
        uv_fix[(1, 14, gz)]  = (0, 14)
    for gy in range(10, 15):              # ponytail spans gy 10-14
        uv_fix[(0, gy, 2)] = (0, 16)     # hair pixel (top of head)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_mage(height=1.8):
    """Male mage NPC. Robed spellcaster with a pointed wizard hat.

    Origin at feet (Y=0). Slightly narrower build than cleric, tall hat adds
    height above gy=15. Long robes hide the legs.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eye sockets (front face only)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))

    # Pointed wizard hat — wide brim narrows to a tip
    fill_box(-2, 16, -2, 5, 1, 4)  # brim, gy=16, 5 wide
    fill_box(-1, 17, -2, 3, 1, 4)  # middle, gy=17, 3 wide
    fill_box(0, 18, -1, 1, 1, 2)   # pointed tip, gy=18, 1 wide

    # Hood/collar
    fill_box(-2, 11, -1, 5, 2, 3)

    # Torso — narrower robe than cleric (gx -3 to 3, 7 wide)
    fill_box(-3, 5, -1, 7, 6, 3)
    # Slight shoulder guards
    fill_box(-4, 9, 0, 1, 2, 1)
    fill_box(4, 9, 0, 1, 2, 1)

    # Belt/sash
    fill_box(-3, 4, -1, 7, 1, 3)

    # Arms (sleeves, same width as cleric)
    fill_box(-4, 7, 0, 1, 3, 1)
    fill_box(4, 7, 0, 1, 3, 1)
    fill_box(-4, 4, 0, 1, 3, 1)
    fill_box(4, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # Legs hidden under robes
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)
    # Boots
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    # Remap back-of-head voxels in eye columns so eye pixel doesn't bleed
    # through to the rear faces.
    uv_fix = {}
    for gz in range(0, 2):
        uv_fix[(-1, 14, gz)] = (0, 14)
        uv_fix[(1, 14, gz)]  = (0, 14)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_rogue(height=1.7):
    """Male rogue NPC. Lean hooded figure with cape, lean armor.

    Origin at feet (Y=0). Similar proportions to archer but darker and
    with a hood and a thin back cape.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eye sockets (front face only)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))

    # Hood — covers top and wraps down sides of head
    fill_box(-2, 15, -2, 5, 2, 4)  # top of hood
    fill_box(-2, 13, 1, 1, 3, 1)   # left side drape
    fill_box(2, 13, 1, 1, 3, 1)    # right side drape

    # Neck
    fill_box(0, 11, 0, 1, 1, 1)

    # Slim torso
    fill_box(-2, 8, -1, 4, 3, 3)   # upper chest (lean)

    # Narrow waist/belt
    fill_box(-1, 6, -1, 3, 2, 3)
    fill_box(-2, 5, -1, 5, 1, 3)   # belt row

    # Hips
    fill_box(-2, 4, -1, 5, 1, 3)

    # Cape at back — thin voxels along gz=2 from gy=4 to gy=10
    for gy in range(4, 11):
        filled.add((-1, gy, 2))
        filled.add((0, gy, 2))
        filled.add((1, gy, 2))

    # Arms (lean, gx=-3 and gx=2 like archer)
    fill_box(-3, 7, 0, 1, 3, 1)
    fill_box(2, 7, 0, 1, 3, 1)
    fill_box(-3, 4, 0, 1, 3, 1)
    fill_box(2, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-3, 3, -1, 1, 1, 2)
    fill_box(2, 3, -1, 1, 1, 2)

    # Legs
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, 0, 1, 2, 1)
    fill_box(1, 0, 0, 1, 2, 1)
    # Boots
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(1, 0, -1, 1, 1, 2)

    # Remap back-of-head voxels in eye columns to skin color.
    uv_fix = {}
    for gz in range(0, 2):
        uv_fix[(-1, 14, gz)] = (0, 14)
        uv_fix[(1, 14, gz)]  = (0, 14)
    # Remap cape voxels at face gy levels to avoid picking up face texture.
    for gy in range(4, 11):
        uv_fix[(-1, gy, 2)] = (0, 8)   # leather armor pixel
        uv_fix[(0, gy, 2)]  = (0, 8)
        uv_fix[(1, gy, 2)]  = (0, 8)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_paladin(height=1.85):
    """Male paladin NPC. Heavy plate armor, broad build, flat-top helm.

    Origin at feet (Y=0). Stockier and wider than the cleric — full plate
    coverage with large pauldrons and thick armored limbs. No hair visible.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (same width as cleric — 5 wide)
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eye sockets (front face only — visor opening)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth/chin slot
    filled.discard((0, 12, -2))

    # Flat-top great helm — full coverage, no hair
    fill_box(-2, 15, -2, 5, 2, 4)  # helm main block (gy=15-16)
    # Visor slit: remove front row at gy=15 to create a narrow eye opening
    for gx in range(-2, 3):
        filled.discard((gx, 15, -2))

    # Wide torso — heavier plate than cleric (gx -3 to 3, 7 wide)
    fill_box(-3, 5, -1, 7, 6, 3)
    # Large pauldrons (shoulder armor — gx -4 with w=2, gx 3 with w=2)
    fill_box(-4, 9, -1, 2, 2, 3)   # left pauldron
    fill_box(3, 9, -1, 2, 2, 3)    # right pauldron

    # Belt/tassets row
    fill_box(-3, 4, -1, 7, 1, 3)

    # Thick armored arms (2 voxels each side: gx=-4,-5 left, gx=4,5 right,
    # but pauldrons only go to gx=-4 so arms are at gx=-4 only — keep
    # consistent with pauldron width; arms attach below pauldron at gy=7-8)
    fill_box(-4, 7, 0, 1, 2, 1)    # left upper arm
    fill_box(4, 7, 0, 1, 2, 1)     # right upper arm
    fill_box(-4, 4, 0, 1, 3, 1)    # left lower arm
    fill_box(4, 4, 0, 1, 3, 1)     # right lower arm
    # Gauntlets
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # Thick armored legs
    fill_box(-2, 2, -1, 2, 2, 2)   # left thigh
    fill_box(1, 2, -1, 2, 2, 2)    # right thigh
    fill_box(-2, 0, -1, 2, 2, 2)   # left greave
    fill_box(1, 0, -1, 2, 2, 2)    # right greave
    # Armored sabatons (boots with extra front coverage)
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    # Remap back-of-head voxels at eye-column gy=14 to skin color so eye
    # glow doesn't bleed to the rear helmet faces.  gz=-1 (eyeball behind
    # the visor slot) keeps the eye color; gz=0 and gz=1 are remapped.
    uv_fix = {}
    for gz in range(0, 2):      # gz=0 and gz=1 (behind front face)
        uv_fix[(-1, 14, gz)] = (0, 14)   # center column = skin/neutral pixel
        uv_fix[(1, 14, gz)]  = (0, 14)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_staff(height=1.2):
    """Staff weapon mesh — tall thin rod with crystal tip.

    Origin at base (Y=0). About 1.2 units tall, 0.06 wide shaft with a
    slightly larger crystal orb at the top.
    """
    mb = MeshBuilder()
    # Shaft — thin rod for most of the height
    add_box(mb, center=(0.0, height * 0.85 * 0.5, 0.0),
            half_extents=(0.03, height * 0.85 * 0.5, 0.03))
    # Crystal/orb on top — slightly larger box
    add_box(mb, center=(0.0, height * 0.82 + 0.09, 0.0),
            half_extents=(0.06, 0.09, 0.06))
    return mb


def gen_turret(height=0.4):
    """Mobile turret bot — compact armored body on tank treads.

    Two track assemblies (left/right), boxy armored housing, barrel, antenna.
    Origin at feet (Y=0). About 0.4 units tall, 0.3 wide.
    """
    mb = MeshBuilder()
    # Left track assembly (long low box)
    add_box(mb, center=(-0.12, 0.04, 0.0), half_extents=(0.03, 0.04, 0.12))
    # Left track wheels (front + rear cylinders)
    add_cylinder(mb, base_center=(-0.12, 0.0, -0.10), radius=0.04, height=0.03, sides=6)
    add_cylinder(mb, base_center=(-0.12, 0.0, 0.08), radius=0.04, height=0.03, sides=6)
    # Right track assembly
    add_box(mb, center=(0.12, 0.04, 0.0), half_extents=(0.03, 0.04, 0.12))
    # Right track wheels
    add_cylinder(mb, base_center=(0.12, 0.0, -0.10), radius=0.04, height=0.03, sides=6)
    add_cylinder(mb, base_center=(0.12, 0.0, 0.08), radius=0.04, height=0.03, sides=6)
    # Armored body housing (sits on top of tracks)
    add_box(mb, center=(0.0, 0.14, 0.0), half_extents=(0.09, 0.05, 0.08))
    # Top turret head (rotatable look)
    add_box(mb, center=(0.0, 0.22, 0.0), half_extents=(0.06, 0.03, 0.06))
    # Barrel — extends forward from turret head
    add_box(mb, center=(0.0, 0.22, -0.14), half_extents=(0.015, 0.015, 0.08))
    # Antenna — thin vertical rod
    add_box(mb, center=(0.04, 0.30, 0.03), half_extents=(0.005, 0.06, 0.005))
    return mb


def gen_gargoyle(height=1.6):
    """Gargoyle — stone ambush enemy. Hunched humanoid with stubby wing plates.
    Origin at feet (Y=0). 14 voxels tall, wider and stockier than skeleton."""
    mb = MeshBuilder()
    vs = height / 14.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Head (hunched forward)
    fill_box(-1, 11, -1, 3, 3, 3)
    # Neck
    fill_box(0, 10, 0, 1, 1, 1)
    # Torso (wide, stocky)
    fill_box(-2, 6, -1, 5, 4, 3)
    # Wing plates (stubby, on back)
    fill_box(-3, 8, 1, 2, 3, 2)
    fill_box(2, 8, 1, 2, 3, 2)
    # Arms (thick)
    fill_box(-3, 5, 0, 1, 4, 1)
    fill_box(3, 5, 0, 1, 4, 1)
    fill_box(-4, 4, -1, 1, 3, 1)
    fill_box(4, 4, -1, 1, 3, 1)
    # Hands/claws
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)
    # Pelvis
    fill_box(-1, 4, 0, 3, 2, 2)
    # Legs (thick)
    fill_box(-2, 2, 0, 2, 3, 1)
    fill_box(1, 2, 0, 2, 3, 1)
    # Feet
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_necromancer(height=2.0):
    """Necromancer — tall hooded skeleton mage. Robed, thin arms.
    Origin at feet (Y=0). 18 voxels tall."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Hood (pointed)
    fill_box(0, 17, 0, 1, 1, 1)
    fill_box(-1, 15, -1, 3, 2, 3)
    # Head inside hood
    fill_box(-1, 13, -1, 3, 2, 2)
    # Neck
    fill_box(0, 12, 0, 1, 1, 1)
    # Shoulders
    fill_box(-3, 11, 0, 7, 1, 2)
    # Upper robe (narrow)
    fill_box(-2, 9, -1, 5, 2, 3)
    # Mid robe (widening)
    fill_box(-2, 6, -1, 5, 3, 3)
    # Lower robe (widest)
    fill_box(-3, 2, -1, 7, 4, 4)
    # Robe hem
    fill_box(-3, 0, -2, 7, 2, 5)
    # Arms (thin, holding staff position)
    fill_box(-3, 8, 0, 1, 3, 1)
    fill_box(3, 8, 0, 1, 3, 1)
    fill_box(-4, 6, -1, 1, 2, 1)
    fill_box(4, 6, -1, 1, 2, 1)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_shaman(height=1.6):
    """Shaman — stocky healer with horned headdress. Broad shoulders.
    Origin at feet (Y=0). 16 voxels tall."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Horns
    fill_box(-2, 15, 0, 1, 1, 1)
    fill_box(2, 15, 0, 1, 1, 1)
    # Headdress (wide crown)
    fill_box(-2, 14, -1, 5, 1, 3)
    # Head
    fill_box(-1, 12, -1, 3, 2, 3)
    # Neck
    fill_box(0, 11, 0, 1, 1, 1)
    # Broad shoulders
    fill_box(-3, 9, -1, 7, 2, 3)
    # Torso (wide)
    fill_box(-2, 6, -1, 5, 3, 3)
    # Pelvis
    fill_box(-2, 5, 0, 5, 1, 2)
    # Arms
    fill_box(-3, 6, 0, 1, 4, 1)
    fill_box(3, 6, 0, 1, 4, 1)
    # Hands
    fill_box(-3, 5, -1, 1, 1, 1)
    fill_box(3, 5, -1, 1, 1, 1)
    # Legs (short, thick)
    fill_box(-2, 2, 0, 2, 3, 1)
    fill_box(1, 2, 0, 2, 3, 1)
    # Feet
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_herald(height=2.2):
    """Herald — tall thin skeleton with open rib cage and extended arms.
    Aura-caster enemy. Origin at feet (Y=0). 20 voxels tall."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Skull (angular)
    fill_box(-1, 17, -1, 3, 3, 3)
    # Jaw
    fill_box(-1, 16, -1, 3, 1, 2)
    # Neck (thin)
    fill_box(0, 15, 0, 1, 1, 1)
    # Shoulders (wide, thin)
    fill_box(-4, 14, 0, 9, 1, 2)
    # Upper chest (ribs, open center)
    fill_box(-3, 12, -1, 2, 2, 3)
    fill_box(2, 12, -1, 2, 2, 3)
    # Spine (visible through open chest)
    fill_box(0, 10, 1, 1, 4, 1)
    # Lower ribs
    fill_box(-2, 10, -1, 2, 2, 3)
    fill_box(1, 10, -1, 2, 2, 3)
    # Pelvis
    fill_box(-1, 8, 0, 3, 2, 2)
    # Arms (long, extended outward)
    fill_box(-4, 11, 0, 1, 3, 1)
    fill_box(4, 11, 0, 1, 3, 1)
    fill_box(-5, 9, -1, 1, 3, 1)
    fill_box(5, 9, -1, 1, 3, 1)
    # Clawed hands
    fill_box(-5, 8, -1, 1, 1, 2)
    fill_box(5, 8, -1, 1, 1, 2)
    # Legs (thin)
    fill_box(-1, 4, 0, 1, 4, 1)
    fill_box(1, 4, 0, 1, 4, 1)
    # Feet
    fill_box(-1, 0, -1, 1, 4, 2)
    fill_box(1, 0, -1, 1, 4, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_sword(length=0.8):
    """Sword weapon — elongated blade + handle.

    Blade extends upward along Y, grip below origin.
    """
    mb = MeshBuilder()
    # Blade — thin wide box extending upward
    add_box(mb, center=(0.0, 0.15, 0.0),
            half_extents=(0.025, 0.3, 0.01))
    # Grip — slightly narrower below blade
    add_box(mb, center=(0.0, -0.2, 0.0),
            half_extents=(0.02, 0.075, 0.02))
    return mb


def gen_dagger(length=0.4):
    """Dagger weapon — short blade + crossguard.

    Shorter and thinner than sword, compact grip.
    """
    mb = MeshBuilder()
    # Blade — shorter than sword
    add_box(mb, center=(0.0, 0.05, 0.0),
            half_extents=(0.02, 0.15, 0.01))
    # Grip
    add_box(mb, center=(0.0, -0.18, 0.0),
            half_extents=(0.0175, 0.06, 0.0175))
    return mb


def gen_axe(length=0.7):
    """Axe weapon — asymmetric axe blade on shaft.

    Wide blade head on left side, butt on right, long haft downward.
    """
    mb = MeshBuilder()
    # Blade head — wide, on left side
    add_box(mb, center=(-0.06, 0.225, 0.0),
            half_extents=(0.06, 0.075, 0.015))
    # Butt — small counterweight on right
    add_box(mb, center=(0.02, 0.22, 0.0),
            half_extents=(0.02, 0.04, 0.02))
    # Haft — long vertical shaft
    add_box(mb, center=(0.0, -0.06, 0.0),
            half_extents=(0.015, 0.24, 0.015))
    return mb


def gen_claymore(length=1.2):
    """Claymore weapon — large two-handed sword.

    Longer blade than sword, wide crossguard, extended grip.
    """
    mb = MeshBuilder()
    # Blade — long and slightly wider than sword
    add_box(mb, center=(0.0, 0.25, 0.0),
            half_extents=(0.03, 0.45, 0.012))
    # Crossguard — wide horizontal bar
    add_box(mb, center=(0.0, -0.05, 0.0),
            half_extents=(0.08, 0.015, 0.015))
    # Grip — long two-handed grip
    add_box(mb, center=(0.0, -0.25, 0.0),
            half_extents=(0.02, 0.12, 0.02))
    return mb


def gen_pistol(length=0.25):
    """Pistol weapon — L-shaped handgun.

    Barrel extends forward along Z, grip hangs below.
    """
    mb = MeshBuilder()
    # Barrel/frame — extends forward along Z
    add_box(mb, center=(0.0, 0.02, 0.05),
            half_extents=(0.02, 0.02, 0.1))
    # Grip — hangs below, angled back
    add_box(mb, center=(0.0, -0.06, -0.05),
            half_extents=(0.02, 0.06, 0.015))
    return mb


def gen_smg(length=0.35):
    """SMG weapon — compact automatic with magazine.

    Longer barrel than pistol, box magazine underneath.
    """
    mb = MeshBuilder()
    # Body/barrel
    add_box(mb, center=(0.0, 0.025, 0.1),
            half_extents=(0.02, 0.025, 0.15))
    # Grip
    add_box(mb, center=(0.0, -0.06, -0.02),
            half_extents=(0.02, 0.06, 0.015))
    # Magazine — box below barrel
    add_box(mb, center=(0.0, -0.04, 0.02),
            half_extents=(0.015, 0.04, 0.01))
    return mb


def gen_carbine(length=0.6):
    """Carbine weapon — long-barreled rifle with stock.

    Extended barrel, buttstock behind, grip below.
    """
    mb = MeshBuilder()
    # Barrel/receiver — long
    add_box(mb, center=(0.0, 0.02, 0.15),
            half_extents=(0.0175, 0.02, 0.225))
    # Stock — extends behind
    add_box(mb, center=(0.0, 0.0, -0.15),
            half_extents=(0.02, 0.025, 0.075))
    # Grip
    add_box(mb, center=(0.0, -0.05, -0.02),
            half_extents=(0.0175, 0.05, 0.015))
    return mb


def gen_revolver(length=0.3):
    """Revolver weapon — barrel + cylinder drum + grip.

    Distinctive cylinder block behind the barrel.
    """
    mb = MeshBuilder()
    # Barrel
    add_box(mb, center=(0.0, 0.02, 0.05),
            half_extents=(0.02, 0.02, 0.075))
    # Cylinder drum — wider block
    add_box(mb, center=(0.0, 0.02, -0.01),
            half_extents=(0.03, 0.03, 0.03))
    # Grip
    add_box(mb, center=(0.0, -0.065, -0.04),
            half_extents=(0.02, 0.065, 0.015))
    return mb


def gen_bow(height=0.8):
    """Bow weapon — D-shape built with add_cylinder for the curved limb.

    The limb is a half-circle arc (cylinder bent into a C), grip at center,
    string connecting the tips. Uses add_cylinder for a smooth curve.
    Two material groups: weapon_bow (brown wood) for the limb and grip,
    and white (pure white tint) for the bowstring.
    """
    mb = MeshBuilder()

    # Wood parts (grip + limb arc) use the bow texture
    mb.set_material("weapon_bow")

    # Grip — the central handle
    add_box(mb, center=(0.0, 0.0, 0.0),
            half_extents=(0.015, 0.04, 0.012))

    # Curved limb — approximate a D-shape arc using small cylinders
    # placed along a semicircle. Cylinders are upright (along Y) and
    # small enough to blend together into a smooth curve.
    num = 10
    R = 0.15  # radius of the D arc
    for i in range(num):
        # Angle from -80 to +80 degrees (almost a semicircle)
        angle = math.radians(-80 + (160 * i / (num - 1)))
        cx = -R * math.cos(angle)  # curves backward (-X)
        cy = R * math.sin(angle)   # vertical spread
        # Each cylinder segment is short and thin
        add_cylinder(mb, base_center=(cx, cy - 0.015, 0.0),
                     radius=0.008, height=0.03, sides=4)

    # String — thin tall box connecting the tips straight across.
    # Uses a distinct white material so it renders visually separate from the wood.
    tip_y = R * math.sin(math.radians(80))
    mb.set_material("white")
    add_box(mb, center=(0.0, 0.0, 0.0),
            half_extents=(0.003, tip_y + 0.01, 0.003))

    return mb


def gen_crossbow(width=0.5):
    """Crossbow weapon — stock with horizontal prod arms.

    Forward-pointing stock, prod arms at the front, bolt channel on top, grip below.
    """
    mb = MeshBuilder()
    # Stock — extends along Z
    add_box(mb, center=(0.0, 0.0, 0.04),
            half_extents=(0.015, 0.015, 0.14))
    # Left prod arm
    add_box(mb, center=(-0.07, 0.02, 0.14),
            half_extents=(0.05, 0.01, 0.02))
    # Right prod arm
    add_box(mb, center=(0.07, 0.02, 0.14),
            half_extents=(0.05, 0.01, 0.02))
    # Bolt channel — thin rail on top
    add_box(mb, center=(0.0, 0.02, 0.06),
            half_extents=(0.008, 0.005, 0.08))
    # Grip
    add_box(mb, center=(0.0, -0.0475, -0.04),
            half_extents=(0.02, 0.0325, 0.02))
    return mb


def gen_throwing_knife(length=0.25):
    """Throwing knife — very thin flat blade + small pommel.

    Extremely thin profile (Z), elongated along Y.
    """
    mb = MeshBuilder()
    # Blade — very thin
    add_box(mb, center=(0.0, 0.03, 0.0),
            half_extents=(0.01, 0.075, 0.0025))
    # Pommel/grip
    add_box(mb, center=(0.0, -0.08, 0.0),
            half_extents=(0.0075, 0.04, 0.0075))
    return mb


def gen_chakram(radius=0.13):
    """Chakram — flat circular throwing blade (a metal ring) lying flat in the XZ plane.

    Reused as BOTH the in-hand weapon mesh and the spinning projectile. A main tube ring forms
    the body; a thinner, slightly larger ring accents the sharpened cutting rim.
    """
    mb = MeshBuilder()
    mb.set_material("prop_iron")
    # Body ring
    add_torus(mb, center=(0.0, 0.0, 0.0), major_r=radius, minor_r=0.022,
              major_segs=20, minor_segs=6)
    # Sharpened outer edge — thin ring just outside the body for a bladed silhouette
    add_torus(mb, center=(0.0, 0.0, 0.0), major_r=radius + 0.012, minor_r=0.006,
              major_segs=20, minor_segs=4)
    return mb


def gen_infinity_chakram(radius=0.05):
    """Infinity Chakram — two linked metal rings forming a figure-8 (∞), lying flat in the XZ
    plane. Reused as BOTH the in-hand weapon mesh and the spinning projectile (legendary chakram).
    The two rings cross at the origin so the silhouette reads as ∞ from above.
    """
    mb = MeshBuilder()
    mb.set_material("prop_iron")
    for cx in (-radius, radius):  # centers ±radius so the two rings overlap at the origin → ∞
        add_torus(mb, center=(cx, 0.0, 0.0), major_r=radius, minor_r=0.016,
                  major_segs=18, minor_segs=6)
        # Thin sharpened rim, like the regular chakram
        add_torus(mb, center=(cx, 0.0, 0.0), major_r=radius + 0.010, minor_r=0.005,
                  major_segs=18, minor_segs=4)
    return mb


def gen_molotov(height=0.25):
    """Molotov cocktail — bottle body + neck.

    Wider bottle body below, narrow neck above.
    """
    mb = MeshBuilder()
    # Bottle body
    add_box(mb, center=(0.0, -0.01, 0.0),
            half_extents=(0.03, 0.06, 0.03))
    # Neck
    add_box(mb, center=(0.0, 0.09, 0.0),
            half_extents=(0.015, 0.03, 0.015))
    return mb


def gen_wand(length=0.35):
    """Wand weapon — thin shaft with crystal tip and pommel.

    Long shaft, gem/crystal at top, small pommel at bottom.
    """
    mb = MeshBuilder()
    # Shaft — long thin rod
    add_box(mb, center=(0.0, -0.025, 0.0),
            half_extents=(0.015, 0.225, 0.015))
    # Crystal tip — wider at top
    add_box(mb, center=(0.0, 0.25, 0.0),
            half_extents=(0.025, 0.05, 0.025))
    # Pommel — small at bottom
    add_box(mb, center=(0.0, -0.25, 0.0),
            half_extents=(0.02, 0.03, 0.02))
    return mb


def gen_web(size=1.0):
    """Flat spider web decoration — thin slab in XZ plane (for legacy/reference)."""
    mb = MeshBuilder()
    hs = size * 0.5
    add_box(mb, center=(0.0, 0.0, 0.0), half_extents=(hs, 0.02, hs))
    return mb


def gen_web_wall(size=1.0):
    """Wall-mounted spider web — upright panel, thin in Z, 1m wide x 1m tall."""
    mb = MeshBuilder()
    hs = size * 0.5
    add_box(mb, center=(0.0, 0.0, 0.0), half_extents=(hs, hs, 0.02))
    return mb


def gen_web_ceiling(size=1.0):
    """Ceiling spider web — same shape as web_wall but rotated 90° around X
    so it lies flat against the ceiling. The texture maps identically to the
    wall version so the web pattern looks correct from below."""
    mb = MeshBuilder()
    hs = size * 0.5
    # web_wall is thin in Z, tall in Y: half_extents = (hs, hs, 0.02)
    # Rotate 90° around X: Y→Z, Z→-Y → half_extents become (hs, 0.02, hs)
    add_box(mb, center=(0.0, 0.0, 0.0), half_extents=(hs, 0.02, hs))
    return mb


def gen_shackles(height=0.8):
    """Wall shackles — bar with two hanging chain links and cuffs."""
    mb = MeshBuilder()
    # Horizontal bar
    add_box(mb, center=(0.0, height, 0.0), half_extents=(0.15, 0.015, 0.015))
    # Left/right chain links
    add_box(mb, center=(-0.08, height * 0.7, 0.0), half_extents=(0.01, height * 0.15, 0.01))
    add_box(mb, center=( 0.08, height * 0.7, 0.0), half_extents=(0.01, height * 0.15, 0.01))
    # Left/right cuffs
    add_box(mb, center=(-0.08, height * 0.5, 0.0), half_extents=(0.025, 0.015, 0.02))
    add_box(mb, center=( 0.08, height * 0.5, 0.0), half_extents=(0.025, 0.015, 0.02))
    return mb


def gen_barrel(radius=0.25, height=0.7):
    """Simple barrel — octagonal cylinder with rim bands."""
    mb = MeshBuilder()
    add_cylinder(mb, base_center=(0, 0, 0), radius=radius, height=height, sides=8)
    add_cylinder(mb, base_center=(0, height * 0.85, 0), radius=radius * 1.05, height=height * 0.05, sides=8)
    add_cylinder(mb, base_center=(0, height * 0.05, 0), radius=radius * 1.05, height=height * 0.05, sides=8)
    return mb


def gen_cage(width=0.8, height=1.5):
    """Hanging cage with vertical bars — semi-transparent look."""
    mb = MeshBuilder()
    hw = width * 0.5
    barR = 0.015
    # Top and bottom rings
    add_box(mb, center=(0.0, height, 0.0), half_extents=(hw, 0.02, hw))
    add_box(mb, center=(0.0, 0.0, 0.0), half_extents=(hw, 0.02, hw))
    # 8 vertical bars around perimeter
    for i in range(8):
        angle = i * math.pi * 2.0 / 8.0
        bx = math.sin(angle) * (hw - barR)
        bz = math.cos(angle) * (hw - barR)
        add_box(mb, center=(bx, height * 0.5, bz), half_extents=(barR, height * 0.5, barR))
    # Chain link hanging from top
    add_box(mb, center=(0.0, height + 0.15, 0.0), half_extents=(0.01, 0.15, 0.01))
    return mb


def gen_bones(spread=0.4):
    """Pile of bones on the ground."""
    mb = MeshBuilder()
    add_box(mb, center=(-0.1, 0.02, 0.0), half_extents=(0.15, 0.02, 0.02))
    add_box(mb, center=(0.05, 0.02, 0.08), half_extents=(0.02, 0.02, 0.12))
    add_box(mb, center=(0.0, 0.04, -0.05), half_extents=(0.12, 0.02, 0.02))
    # Skull
    add_box(mb, center=(0.08, 0.06, 0.0), half_extents=(0.04, 0.04, 0.04))
    return mb


def gen_brazier(height=0.6):
    """Standing brazier/fire bowl on a thin post."""
    mb = MeshBuilder()
    add_cylinder(mb, base_center=(0, 0, 0), radius=0.04, height=height * 0.75, sides=6)
    add_cylinder(mb, base_center=(0, 0, 0), radius=0.12, height=0.04, sides=6)
    add_cylinder(mb, base_center=(0, height * 0.7, 0), radius=0.1, height=height * 0.2, sides=6)
    return mb


def gen_butcher(height=2.5):
    """Large demon boss — The Butcher. Massive build, horns, red-tinted.

    Origin at feet (Y=0). Much larger than normal humanoids.
    """
    mb = MeshBuilder()
    vs = height / 20.0  # 20 voxels tall for more detail at large scale
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Massive head ---
    fill_box(-3, 16, -3, 6, 4, 5)
    # Jaw (wide)
    fill_box(-2, 15, -3, 4, 1, 4)
    # Deep eye sockets
    filled.discard((-2, 18, -3))
    filled.discard((1, 18, -3))
    filled.discard((-2, 18, -2))
    filled.discard((1, 18, -2))
    # Mouth snarl
    filled.discard((-1, 15, -3))
    filled.discard((0, 15, -3))

    # Horns (curving up and out)
    fill_box(-4, 19, -1, 1, 2, 1)  # left horn base
    filled.add((-5, 20, -1))        # left horn tip
    fill_box(3, 19, -1, 1, 2, 1)   # right horn base
    filled.add((4, 20, -1))         # right horn tip

    # Neck (thick)
    fill_box(-1, 14, -1, 3, 2, 2)

    # --- Massive torso ---
    fill_box(-4, 8, -2, 8, 6, 4)
    # Barrel chest protrusion
    fill_box(-3, 10, -3, 6, 3, 1)
    # Huge shoulder mounds
    fill_box(-5, 12, -1, 1, 2, 3)
    fill_box(4, 12, -1, 1, 2, 3)

    # Belt / waist
    fill_box(-4, 7, -2, 8, 1, 4)

    # --- Thick arms ---
    fill_box(-5, 9, 0, 1, 4, 1)   # left upper arm
    fill_box(4, 9, 0, 1, 4, 1)    # right upper arm
    fill_box(-5, 5, 0, 1, 4, 1)   # left lower arm
    fill_box(4, 5, 0, 1, 4, 1)    # right lower arm
    # Big fists
    fill_box(-6, 4, -1, 2, 2, 2)
    fill_box(4, 4, -1, 2, 2, 2)

    # --- Powerful legs ---
    fill_box(-3, 3, -1, 3, 4, 3)  # left thigh
    fill_box(1, 3, -1, 3, 4, 3)   # right thigh
    fill_box(-3, 0, -1, 3, 3, 3)  # left shin
    fill_box(1, 0, -1, 3, 3, 3)   # right shin
    # Hooves
    fill_box(-3, 0, -2, 3, 1, 4)
    fill_box(1, 0, -2, 3, 1, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_hellhound(height=1.2):
    """Hellhound — quadruped canine demon. Low to the ground, long body, snarling jaws.

    Origin at feet (Y=0). Oriented along Z axis (nose at -Z, tail at +Z).
    Uses spider rig for 4-legged animation but with canine proportions.
    """
    mb = MeshBuilder()
    vs = height / 10.0  # 10 voxels tall, compact
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (snarling) ---
    fill_box(-2, 6, -5, 4, 3, 3)   # skull
    fill_box(-1, 6, -7, 2, 2, 2)   # snout/jaw
    # Ears
    filled.add((-2, 9, -4))
    filled.add((1, 9, -4))
    # Eye sockets
    filled.discard((-2, 8, -5))
    filled.discard((1, 8, -5))

    # --- Neck (thick, sloping down) ---
    fill_box(-2, 5, -3, 4, 3, 2)

    # --- Torso (long, barrel-shaped) ---
    fill_box(-2, 4, -1, 4, 4, 6)   # main body
    fill_box(-3, 5, 0, 6, 2, 4)    # ribcage width

    # --- Haunches (rear, slightly raised) ---
    fill_box(-2, 4, 5, 4, 3, 2)

    # --- Tail ---
    filled.add((0, 6, 7))
    filled.add((0, 7, 8))
    filled.add((0, 7, 9))

    # --- Front legs ---
    fill_box(-3, 0, -2, 2, 5, 2)   # left front
    fill_box(1, 0, -2, 2, 5, 2)    # right front
    # Paws
    fill_box(-3, 0, -3, 2, 1, 3)
    fill_box(1, 0, -3, 2, 1, 3)

    # --- Rear legs ---
    fill_box(-3, 0, 4, 2, 4, 2)    # left rear
    fill_box(1, 0, 4, 2, 4, 2)     # right rear
    # Paws
    fill_box(-3, 0, 3, 2, 1, 3)
    fill_box(1, 0, 3, 2, 1, 3)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_wraith(height=2.0):
    """Tomb Wraith — ghostly legless floating torso with trailing arms.
    Origin at feet (Y=0). 18 voxels tall. No legs, just a wispy tail."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Hood point
    fill_box(0, 17, 0, 1, 1, 1)
    # Hood dome
    fill_box(-1, 15, -1, 3, 2, 3)
    # Head inside hood
    fill_box(-1, 13, -1, 3, 2, 2)
    # Shoulder shroud
    fill_box(-2, 12, 0, 5, 1, 2)
    # Torso — narrow with open center spine (gaps for ghostly look)
    fill_box(-1, 6, 0, 3, 6, 2)
    # Remove center voxels to create spine gaps
    for gy in range(7, 12, 2):
        filled.discard((0, gy, 0))
        filled.discard((0, gy, 1))
    # Arms — very long, thin
    # Upper arms
    fill_box(-2, 9, 0, 1, 3, 1)
    fill_box(2, 9, 0, 1, 3, 1)
    # Lower arms (extending very low)
    fill_box(-2, 1, 0, 1, 5, 1)
    fill_box(2, 1, 0, 1, 5, 1)
    # Wispy tail — tapers from pelvis, no legs
    fill_box(0, 0, 0, 1, 3, 1)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_sentinel(height=1.8):
    """Catacomb Sentinel — armored shield-bearing undead guard.
    Origin at feet (Y=0). 16 voxels tall. Solid plated torso."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Helmet (flat top, wider than skull)
    fill_box(-2, 13, -2, 4, 3, 4)
    # Thick neck
    fill_box(-1, 12, 0, 2, 1, 2)
    # Solid plated torso
    fill_box(-2, 7, -1, 5, 5, 3)
    # Left shield arm (bulky)
    fill_box(-4, 5, 0, 2, 6, 1)
    # Right arm — upper
    fill_box(3, 9, 0, 1, 3, 1)
    # Right arm — lower
    fill_box(3, 6, 0, 1, 3, 1)
    # Armored pelvis
    fill_box(-2, 5, 0, 5, 2, 2)
    # Thick legs
    fill_box(-2, 2, 0, 2, 3, 1)
    fill_box(1, 2, 0, 2, 3, 1)
    # Wide boots
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_cave_troll(height=2.2):
    """Cave Troll — hunched brutish creature with massive shoulders.
    Origin at feet (Y=0). 18 voxels tall. Short legs, big fists."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Small head (sunk between shoulders)
    fill_box(-1, 15, -1, 3, 2, 3)
    # No visible neck — head sits directly on shoulders
    # Massive shoulders
    fill_box(-4, 13, -1, 9, 2, 3)
    # Barrel torso (solid fill)
    fill_box(-3, 8, -2, 7, 5, 4)
    # Thick arms — upper
    fill_box(-5, 7, 0, 2, 4, 1)
    fill_box(4, 7, 0, 2, 4, 1)
    # Thick arms — lower
    fill_box(-5, 3, 0, 2, 4, 1)
    fill_box(4, 3, 0, 2, 4, 1)
    # Big fists
    fill_box(-6, 2, -1, 2, 2, 2)
    fill_box(4, 2, -1, 2, 2, 2)
    # Wide pelvis
    fill_box(-3, 6, -1, 7, 2, 3)
    # Short thick legs
    fill_box(-3, 2, 0, 3, 3, 2)
    fill_box(1, 2, 0, 3, 3, 2)
    # Wide flat feet
    fill_box(-3, 0, -1, 3, 2, 3)
    fill_box(1, 0, -1, 3, 2, 3)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_pit_fiend(height=2.4):
    """Pit Fiend — classic winged demon. Massive bat wings (real 3D geometry),
    muscular torso, curved horns, barrel chest, cloven hooves. Balrog-style.
    Origin at feet (Y=0). 24 voxels tall, 15 wide for wing span."""
    mb = MeshBuilder()
    vs = height / 24.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Curved horns (gy 22-23) — swept back
    fill_box(-3, 22, 0, 1, 2, 1)      # left horn base
    filled.add((-4, 23, 1))            # left horn tip (swept back)
    fill_box(2, 22, 0, 1, 2, 1)       # right horn base
    filled.add((3, 23, 1))             # right horn tip

    # Head (gy 18-21) — 5x4x4, imposing
    fill_box(-2, 18, -2, 5, 4, 4)
    # Deep eye sockets
    filled.discard((-2, 20, -2))
    filled.discard((2, 20, -2))

    # Thick neck (gy 16-17)
    fill_box(-1, 16, -1, 3, 2, 3)

    # Massive torso (gy 10-15) — 7 wide, 4 deep
    fill_box(-3, 10, -2, 7, 6, 4)

    # Barrel chest protrusion (front face, gy 12-14)
    fill_box(-2, 12, -3, 5, 3, 1)

    # Wing roots on upper back (gy 13-15) — anchor geometry
    fill_box(-4, 13, 2, 1, 3, 2)      # left wing root
    fill_box(3, 13, 2, 1, 3, 2)       # right wing root

    # Left wing — MAIN FEATURE — real 3D bat wings
    # Bone struts (2 voxels deep for visual mass)
    fill_box(-7, 14, 1, 3, 2, 2)      # upper strut
    fill_box(-7, 12, 2, 3, 2, 2)      # lower strut
    fill_box(-6, 11, 3, 2, 1, 2)      # wing tip
    # Membrane panels (fill gaps between struts)
    fill_box(-6, 13, 1, 2, 1, 1)      # upper membrane
    fill_box(-5, 12, 1, 1, 2, 1)      # inner membrane
    fill_box(-7, 13, 3, 1, 1, 1)      # outer membrane

    # Right wing (mirrored)
    fill_box(4, 14, 1, 3, 2, 2)
    fill_box(4, 12, 2, 3, 2, 2)
    fill_box(4, 11, 3, 2, 1, 2)
    fill_box(4, 13, 1, 2, 1, 1)
    fill_box(4, 12, 1, 1, 2, 1)
    fill_box(6, 13, 3, 1, 1, 1)

    # Arms — upper (gy 11-14)
    fill_box(-4, 11, -1, 1, 4, 2)
    fill_box(3, 11, -1, 1, 4, 2)
    # Arms — lower (gy 7-10)
    fill_box(-4, 7, -1, 1, 4, 2)
    fill_box(3, 7, -1, 1, 4, 2)
    # Fists (2x2x2)
    fill_box(-5, 6, -1, 2, 2, 2)
    fill_box(3, 6, -1, 2, 2, 2)

    # Tail — trailing from lower back
    filled.add((0, 9, 2))
    filled.add((0, 8, 3))
    filled.add((0, 7, 3))
    filled.add((0, 6, 4))
    filled.add((0, 5, 4))

    # Legs (gy 4-8) — thick thighs
    fill_box(-3, 4, -1, 3, 5, 3)
    fill_box(1, 4, -1, 3, 5, 3)
    # Calves (gy 1-3)
    fill_box(-2, 1, -1, 2, 3, 2)
    fill_box(1, 1, -1, 2, 3, 2)
    # Cloven hooves (gy 0)
    fill_box(-3, 0, -2, 3, 1, 4)
    fill_box(1, 0, -2, 3, 1, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_hellforge_smith(height=2.0):
    """Hellforge Smith — hunched demonic blacksmith. Stocky, wide shoulders,
    massive hammer arm (right), iron apron, forge bellows on back.
    Shorter and wider than butcher. Origin at feet (Y=0). 18 voxels tall."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (gy 15-16) — small, sunken between shoulders
    fill_box(-1, 15, -1, 3, 2, 3)
    # Brow ridge / visor (gy 16)
    fill_box(-2, 16, -2, 5, 1, 1)
    # Eye sockets
    filled.discard((-1, 16, -2))
    filled.discard((1, 16, -2))

    # Massive shoulder hump (gy 13-14) — widest part, hunched look
    fill_box(-5, 13, -1, 11, 2, 3)

    # Broad torso (gy 8-12)
    fill_box(-4, 8, -2, 9, 5, 5)

    # Iron apron on front (gy 6-11)
    fill_box(-3, 6, -3, 7, 6, 1)

    # Forge bellows on back (gy 9-12)
    fill_box(-3, 9, 3, 2, 4, 1)       # left bellows
    fill_box(1, 9, 3, 2, 4, 1)        # right bellows
    fill_box(-1, 10, 3, 2, 2, 1)      # bellows connector

    # Left arm — normal (gy 5-12)
    fill_box(-5, 9, -1, 1, 4, 2)      # upper
    fill_box(-5, 5, -1, 1, 4, 2)      # lower
    fill_box(-6, 4, -1, 2, 2, 2)      # left fist

    # Right arm — HAMMER ARM (oversized, gy 5-13)
    fill_box(4, 9, -1, 2, 4, 2)       # upper arm (thicker)
    fill_box(4, 5, -1, 2, 4, 2)       # lower arm (thicker)
    # Anvil fist / hammer head (3x3x3)
    fill_box(4, 3, -2, 3, 3, 3)

    # Wide pelvis (gy 5-7)
    fill_box(-4, 5, -1, 9, 3, 3)

    # Short thick legs (gy 2-4)
    fill_box(-3, 2, -1, 3, 3, 2)
    fill_box(1, 2, -1, 3, 3, 2)

    # Heavy boots (gy 0-1)
    fill_box(-4, 0, -2, 4, 2, 4)
    fill_box(1, 0, -2, 4, 2, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_succubus(height=1.4):
    """Succubus — cute harpy demon. Hourglass feminine body with bat wings (limbs)
    and dangling talons (limbs). Curved horns, whip tail, hair detail.
    Body has mass and curves: 5-wide chest, 3-wide waist, 5-wide hips.
    16 voxels tall. Origin at feet (Y=0)."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Hair (gy 15) — frames the horns, adds character ---
    fill_box(-1, 15, -2, 3, 1, 4)

    # --- Head (gy 12-14, 3x3 — smaller relative to 5-wide body) ---
    fill_box(-1, 13, -1, 3, 2, 3)   # upper skull
    fill_box(-1, 12, -1, 3, 1, 2)   # lower face/jaw
    # Eye sockets (gy 14, front face)
    filled.discard((-1, 14, -1))
    filled.discard((1, 14, -1))
    # Mouth carved
    filled.discard((0, 12, -1))

    # Horns — swept back and up from above hair (gy 16-17)
    filled.add((-2, 16, 0))   # left horn base
    filled.add((-3, 17, 1))   # left horn tip (swept back)
    filled.add((2, 16, 0))    # right horn base
    filled.add((3, 17, 1))    # right horn tip

    # Neck — thin (gy 11)
    fill_box(0, 11, 0, 1, 1, 1)

    # --- Chest (gy 8-10, 5 wide — the widest upper body) ---
    fill_box(-2, 8, -1, 5, 3, 3)    # main chest block
    fill_box(-2, 8, -2, 5, 2, 1)    # bust protrusion (front, 2 rows deep)
    fill_box(-2, 9, -2, 5, 1, 1)    # upper bust line
    # Shoulder nubs for wing limb attachment
    fill_box(-3, 10, 0, 1, 1, 1)
    fill_box(3, 10, 0, 1, 1, 1)

    # --- Narrow waist (gy 7, 3 wide — the pinch) ---
    fill_box(-1, 7, -1, 3, 1, 2)

    # --- Wide hips (gy 5-6, 5 wide — wider than chest = curvy) ---
    fill_box(-2, 5, -1, 5, 2, 3)

    # --- Tapered pelvis (gy 3-4, 3 wide — narrowing gracefully) ---
    fill_box(-1, 3, -1, 3, 2, 2)

    # --- Lower taper (gy 1-2, 1-2 wide — where talons attach) ---
    fill_box(0, 1, 0, 1, 2, 1)
    filled.add((-1, 2, 0))    # slight width at top of taper
    filled.add((1, 2, 0))

    # --- Body terminus (gy 0) — talon limbs hang from here ---
    filled.add((0, 0, 0))

    # --- Tail (whip, 5 voxels trailing back from hip level) ---
    filled.add((0, 6, 2))
    filled.add((0, 5, 3))
    filled.add((0, 4, 3))
    filled.add((0, 3, 4))
    filled.add((0, 2, 4))     # tail tip

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_abyssal_titan(height=2.8):
    """Abyssal Titan — massive void colossus with crystal spikes.
    Origin at feet (Y=0). 22 voxels tall. Enormous build."""
    mb = MeshBuilder()
    vs = height / 22.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Head
    fill_box(-2, 18, -2, 5, 4, 4)
    # Crystal spikes (crown)
    filled.add((-2, 22, 0))
    filled.add((0, 22, 0))
    filled.add((2, 22, 0))
    # Thick neck
    fill_box(-1, 16, -1, 3, 2, 3)
    # Crystal shoulder spikes
    fill_box(-4, 17, 0, 1, 2, 1)
    fill_box(4, 17, 0, 1, 2, 1)
    # Massive torso (solid)
    fill_box(-4, 9, -2, 9, 7, 5)
    # Huge arms — upper
    fill_box(-6, 10, 0, 2, 5, 1)
    fill_box(5, 10, 0, 2, 5, 1)
    # Huge arms — lower
    fill_box(-6, 5, 0, 2, 5, 1)
    fill_box(5, 5, 0, 2, 5, 1)
    # Fists
    fill_box(-7, 4, -1, 2, 2, 2)
    fill_box(5, 4, -1, 2, 2, 2)
    # Wide pelvis
    fill_box(-4, 7, -1, 9, 2, 3)
    # Massive legs
    fill_box(-4, 2, -1, 3, 5, 3)
    fill_box(2, 2, -1, 3, 5, 3)
    # Enormous feet
    fill_box(-4, 0, -2, 3, 2, 4)
    fill_box(2, 0, -2, 3, 2, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_entropy_weaver(height=2.0):
    """Entropy Weaver — spindly hooded void spellcaster with extra long arms.
    Origin at feet (Y=0). 18 voxels tall. Robed lower body widens downward."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Hood point
    fill_box(0, 17, 0, 1, 1, 1)
    # Wide cowl
    fill_box(-2, 15, -1, 5, 2, 3)
    # Head inside cowl
    fill_box(-1, 13, 0, 2, 2, 2)
    # Wide shoulder drape
    fill_box(-3, 12, 0, 7, 1, 2)
    # Narrow torso with rune border — fill outer ring for top 2 rows
    # Full torso base
    fill_box(-1, 8, 0, 3, 4, 2)
    # Rune border: fill wider then keep only outer ring for Y=10-11
    fill_box(-2, 10, -1, 5, 2, 2)
    # Remove inner voxels of the wider border to leave just the ring
    for ry in [10, 11]:
        for rx in [-1, 0, 1]:
            filled.discard((rx, ry, -1))
    # Extra long arms — upper
    fill_box(-3, 9, 0, 1, 3, 1)
    fill_box(3, 9, 0, 1, 3, 1)
    # Extra long arms — lower (reaching very low)
    fill_box(-3, 2, 0, 1, 5, 1)
    fill_box(3, 2, 0, 1, 5, 1)
    # Claw fingers
    fill_box(-3, 0, 0, 1, 2, 1)
    fill_box(3, 0, 0, 1, 2, 1)
    # Robe — widens from 3 to 5 to 7
    fill_box(-1, 5, -1, 3, 1, 3)  # Y=5, width 3
    fill_box(-2, 3, -1, 5, 2, 3)  # Y=3-4, width 5
    fill_box(-3, 2, -1, 7, 1, 3)  # Y=2, width 7
    # Robe hem
    fill_box(-3, 0, -2, 7, 2, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


# ---------------------------------------------------------------------------
# Type registry
# ---------------------------------------------------------------------------

def gen_skeleton_arm(height=1.8):
    """Skeleton arm — bony arm with fist. Origin near elbow."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Shoulder ball
    fill_box(0, 5, 0, 1, 1, 1)
    # Upper arm (2 voxels tall)
    fill_box(0, 3, 0, 1, 2, 1)
    # Lower arm (3 voxels tall)
    fill_box(0, 0, 0, 1, 3, 1)
    # Fist — compact cube, not flat like a foot
    fill_box(0, -1, 0, 1, 1, 1)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_skeleton_leg(height=1.8):
    """Skeleton leg — extracted from gen_humanoid. Origin at hip (top).
    Includes thigh, shin, and foot voxels."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Thigh (2 voxels tall)
    fill_box(0, 2, 0, 1, 2, 1)
    # Shin (2 voxels tall)
    fill_box(0, 0, 0, 1, 2, 1)
    # Foot
    fill_box(0, 0, -1, 1, 1, 2)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_bat_wing(wingspan=1.0):
    """Bat wing — wider membrane shape with multiple voxels for texture detail."""
    mb = MeshBuilder()
    vs = wingspan / 8.0  # 8 voxels wide
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Wing membrane — tapered shape, wider at base narrowing to tip
    fill_box(0, 0, 0, 8, 1, 4)   # main membrane (8 wide, 1 tall, 4 deep)
    fill_box(1, 0, 4, 5, 1, 2)   # inner extension
    fill_box(2, 0, -1, 4, 1, 1)  # front edge
    # Wing finger bones (ridges along the membrane)
    fill_box(0, 1, 1, 1, 1, 1)   # base bone
    fill_box(3, 1, 2, 1, 1, 1)   # mid bone
    fill_box(6, 1, 1, 1, 1, 1)   # tip bone
    ox = -0.5 * vs
    oz = -2.5 * vs  # center depth-wise
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_pit_fiend_wing(size=0.4):
    """Pit Fiend wing — small triangular plate, D2 Pit Lord style.
    Wide in X, tall in Y, thin in Z (1 voxel) — flat face visible from front.
    Y-axis rotation sweeps it forward/back."""
    mb = MeshBuilder()
    vs = size / 4.0  # 4 voxels tall
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Triangular plate: wide base (3 in X), tapers to 1 at top, 1 deep (Z)
    fill_box(0, 0, 0, 3, 1, 1)   # base row — 3 wide
    fill_box(0, 1, 0, 3, 1, 1)   # second row — 3 wide
    fill_box(0, 2, 0, 2, 1, 1)   # third row — 2 wide (taper)
    fill_box(0, 3, 0, 1, 1, 1)   # tip — 1 wide
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_butcher_arm(height=2.5):
    """Butcher thick demon arm — extracted from gen_butcher. Origin at shoulder."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Upper arm (4 voxels tall, 1 wide)
    fill_box(0, 4, 0, 1, 4, 1)
    # Lower arm (4 voxels tall)
    fill_box(0, 0, 0, 1, 4, 1)
    # Big fist (2x2x2)
    fill_box(-1, -1, -1, 2, 2, 2)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_butcher_leg(height=2.5):
    """Butcher thick demon leg — extracted from gen_butcher. Origin at hip."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Thigh (3x4x3)
    fill_box(-1, 3, -1, 3, 4, 3)
    # Shin (3x3x3)
    fill_box(-1, 0, -1, 3, 3, 3)
    # Hoof
    fill_box(-1, 0, -2, 3, 1, 4)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_bat_foot():
    """Bat foot/talon — small curved claw shape."""
    mb = MeshBuilder()
    vs = 0.05  # small voxel size
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Ankle joint
    fill_box(0, 2, 0, 1, 1, 1)
    # Three toes spreading forward
    fill_box(-1, 0, -1, 1, 2, 1)  # left toe
    fill_box(0, 0, -2, 1, 2, 1)   # center toe
    fill_box(1, 0, -1, 1, 2, 1)   # right toe
    # Back claw
    fill_box(0, 0, 1, 1, 1, 1)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_andariel(height=2.0):
    """Andariel — spider-woman demon boss. Tall, narrow waist, wide chitin shoulders,
    elongated clawed arms, multiple eye sockets. More monster than human.
    Origin at feet (Y=0). Uses 18 voxels tall for detail."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (angular, predatory) ---
    fill_box(-2, 15, -2, 5, 3, 4)   # main skull
    fill_box(-1, 14, -2, 3, 1, 3)   # narrow jaw with fangs
    # 4 eye sockets (front face)
    filled.discard((-1, 16, -2))     # upper left eye
    filled.discard((1, 16, -2))      # upper right eye
    filled.discard((-1, 15, -2))     # lower left eye
    filled.discard((1, 15, -2))      # lower right eye
    # Mouth/fang gap
    filled.discard((0, 14, -2))

    # --- Neck (thin) ---
    fill_box(0, 13, 0, 1, 1, 1)

    # --- Upper torso — wide chitin carapace ---
    fill_box(-3, 10, -1, 7, 3, 3)   # broad chest plate
    # Spiked shoulder pauldrons
    fill_box(-4, 11, -1, 1, 2, 3)   # left shoulder spike
    fill_box(4, 11, -1, 1, 2, 3)    # right shoulder spike
    fill_box(-5, 12, 0, 1, 1, 1)    # left spike tip
    fill_box(5, 12, 0, 1, 1, 1)     # right spike tip

    # --- Narrow waist (spider-like constriction) ---
    fill_box(-1, 8, 0, 3, 2, 2)     # thin waist

    # --- Wide abdomen (spider-like) ---
    fill_box(-3, 5, -1, 7, 3, 4)    # bulbous lower body
    fill_box(-2, 4, -1, 5, 1, 3)    # taper

    # --- Long clawed arms ---
    fill_box(-4, 9, 0, 1, 3, 1)     # left upper arm
    fill_box(4, 9, 0, 1, 3, 1)      # right upper arm
    fill_box(-5, 6, 0, 1, 3, 1)     # left lower arm (longer, angled out)
    fill_box(5, 6, 0, 1, 3, 1)      # right lower arm
    # Clawed hands
    fill_box(-5, 5, -1, 1, 1, 1)    # left claw
    fill_box(5, 5, -1, 1, 1, 1)     # right claw

    # --- Legs (thin, insectoid) ---
    fill_box(-2, 2, 0, 1, 2, 1)     # left thigh
    fill_box(2, 2, 0, 1, 2, 1)      # right thigh
    fill_box(-2, 0, 0, 1, 2, 1)     # left shin
    fill_box(2, 0, 0, 1, 2, 1)      # right shin
    # Pointed feet
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(2, 0, -1, 1, 1, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_humanoid_torso(height=1.8):
    """Skeleton torso only — arms and legs removed for limb system animation."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Skull
    fill_box(-2, 12, -2, 5, 4, 4)
    fill_box(-1, 11, -2, 3, 1, 3)
    filled.discard((-1, 14, -2)); filled.discard((1, 14, -2))
    filled.discard((0, 13, -2)); filled.discard((0, 11, -2))
    # Neck
    fill_box(0, 10, 0, 1, 1, 1)
    # Rib cage
    fill_box(0, 5, 1, 1, 5, 1)
    for ry in [6, 7, 9]:
        fill_box(-2, ry, 0, 2, 1, 2)
        fill_box(1, ry, 0, 2, 1, 2)
    # Shoulder bones (attachment points for arm limbs)
    fill_box(-3, 9, 0, 1, 1, 1)
    fill_box(3, 9, 0, 1, 1, 1)
    # Pelvis (attachment point for leg limbs)
    fill_box(-1, 4, 0, 3, 1, 1)
    # NO arms, NO legs — limb system provides these
    ox = -0.5 * vs; oz = -0.5 * vs
    # UV overrides: prevent eye color bleeding to back-of-head voxels.
    # Eye sockets are at gx=-1,+1 gy=14 gz=-2 (front face removed).
    # Remap back-of-head voxels at same (gx,gy) to a non-eye pixel.
    uv_fix = {}
    for gz in range(0, 2):  # gz=0,1 are behind the eye — gz=-1 is the visible eye, keep it
        uv_fix[(-1, 14, gz)] = (0, 14)
        uv_fix[( 1, 14, gz)] = (0, 14)
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb

def gen_human_torso(height=1.8):
    """Human NPC torso only — arms and legs removed for limb system animation."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Head
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)
    filled.discard((-1, 14, -2)); filled.discard((1, 14, -2))
    filled.discard((0, 12, -2))
    # Neck
    fill_box(0, 11, 0, 1, 1, 1)
    # Torso
    fill_box(-2, 6, -1, 5, 5, 3)
    fill_box(-3, 9, -1, 1, 2, 3)  # left shoulder pad
    fill_box(3, 9, -1, 1, 2, 3)   # right shoulder pad
    # Belt
    fill_box(-2, 5, -1, 5, 1, 3)
    # Hip connectors (slim pelvis for leg attachment)
    fill_box(-2, 4, -1, 5, 1, 2)
    # NO arms, NO legs
    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_butcher_torso(height=2.5):
    """Butcher boss torso only — arms and legs removed for limb system animation."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Head
    fill_box(-3, 16, -3, 6, 4, 5)
    fill_box(-2, 15, -3, 4, 1, 4)
    filled.discard((-2, 18, -3)); filled.discard((1, 18, -3))
    filled.discard((-2, 18, -2)); filled.discard((1, 18, -2))
    filled.discard((-1, 15, -3)); filled.discard((0, 15, -3))
    # Horns
    fill_box(-4, 19, -1, 1, 2, 1); filled.add((-5, 20, -1))
    fill_box(3, 19, -1, 1, 2, 1); filled.add((4, 20, -1))
    # Neck
    fill_box(-1, 14, -1, 3, 2, 2)
    # Massive torso
    fill_box(-4, 8, -2, 8, 6, 4)
    fill_box(-3, 10, -3, 6, 3, 1)
    # Shoulder mounds (attachment points for arm limbs)
    fill_box(-5, 12, -1, 1, 2, 3)
    fill_box(4, 12, -1, 1, 2, 3)
    # Belt / waist
    fill_box(-4, 7, -2, 8, 1, 4)
    # Hip area (attachment for leg limbs)
    fill_box(-3, 6, -1, 7, 1, 3)
    # NO arms, NO legs, NO fists, NO hooves
    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

# ---------------------------------------------------------------------------
# Helper: add a torus to a MeshBuilder
# ---------------------------------------------------------------------------

def add_torus(mb, center, major_r, minor_r, major_segs=12, minor_segs=6):
    """Add a torus (donut) centered at `center`, lying flat in the XZ plane.

    major_r: distance from center of torus to center of tube
    minor_r: radius of the tube itself
    major_segs: segments around the ring
    minor_segs: segments around the tube cross-section
    """
    cx, cy, cz = center
    # Build vertex grid [major_segs][minor_segs]
    grid = []
    for i in range(major_segs):
        theta = 2.0 * math.pi * i / major_segs
        cos_t, sin_t = math.cos(theta), math.sin(theta)
        ring_center_x = cx + major_r * cos_t
        ring_center_z = cz + major_r * sin_t
        row = []
        for j in range(minor_segs):
            phi = 2.0 * math.pi * j / minor_segs
            cos_p, sin_p = math.cos(phi), math.sin(phi)
            # Point on tube surface
            x = ring_center_x + minor_r * cos_p * cos_t
            y = cy + minor_r * sin_p
            z = ring_center_z + minor_r * cos_p * sin_t
            vi = mb.add_vert(x, y, z)
            # Normal points outward from tube center
            nx = cos_p * cos_t
            ny = sin_p
            nz = cos_p * sin_t
            ni = mb.add_normal(nx, ny, nz)
            u = i / major_segs
            v = j / minor_segs
            ti = mb.add_uv(u, v)
            row.append((vi, ti, ni))
        grid.append(row)

    # Stitch quads (as triangle pairs)
    for i in range(major_segs):
        i2 = (i + 1) % major_segs
        for j in range(minor_segs):
            j2 = (j + 1) % minor_segs
            a = grid[i][j]
            b = grid[i2][j]
            c = grid[i2][j2]
            d = grid[i][j2]
            mb.add_tri(a[0], b[0], c[0], a[1], b[1], c[1], a[2], b[2], c[2])
            mb.add_tri(a[0], c[0], d[0], a[1], c[1], d[1], a[2], c[2], d[2])


# ---------------------------------------------------------------------------
# Equipment, props, and projectile meshes
# ---------------------------------------------------------------------------

def gen_helmet(height=0.3, bulk=1.0):
    """Helmet — simple round dome (half-sphere).

    bulk scales the dome radius uniformly (LIGHT<1.0 = slimmer cap, HEAVY>1.0 = chunkier plate).
    At bulk=1.0 the output is byte-identical to the original.
    """
    mb = MeshBuilder()
    r = 0.15 * bulk  # overall dome radius; bulk=1.0 → same as original 0.15
    segs = 10
    rings = 4
    # Bottom ring vertices (equator)
    prev_row = []
    for j in range(segs):
        theta = 2.0 * math.pi * j / segs
        vi = mb.add_vert(r * math.cos(theta), 0, r * math.sin(theta))
        ni = mb.add_normal(math.cos(theta), 0, math.sin(theta))
        ti = mb.add_uv(j / segs, 0)
        prev_row.append((vi, ti, ni))
    # Build rings up to the top
    for i in range(1, rings + 1):
        phi = (math.pi * 0.5) * i / rings
        ring_r = r * math.cos(phi)
        ring_y = r * math.sin(phi)
        row = []
        for j in range(segs):
            theta = 2.0 * math.pi * j / segs
            x, z = ring_r * math.cos(theta), ring_r * math.sin(theta)
            nx, ny, nz = _normalize((x, ring_y, z))
            vi = mb.add_vert(x, ring_y, z)
            ni = mb.add_normal(nx, ny, nz)
            ti = mb.add_uv(j / segs, i / rings)
            row.append((vi, ti, ni))
        for j in range(segs):
            j2 = (j + 1) % segs
            a, b = prev_row[j], prev_row[j2]
            c, d = row[j2], row[j]
            mb.add_tri(a[0], c[0], b[0], a[1], c[1], b[1], a[2], c[2], b[2])
            mb.add_tri(a[0], d[0], c[0], a[1], d[1], c[1], a[2], d[2], c[2])
        prev_row = row
    return mb

def gen_armor(height=0.5, bulk=1.0):
    """Body armor — chest plate with shoulder pauldrons and belt.

    bulk scales plate depth (Z) and pauldron radius/height (LIGHT<1.0 = cloth-thin,
    HEAVY>1.0 = thick plate with larger pauldrons).  X/Y silhouette is unchanged so
    the piece still fits the character frame.  At bulk=1.0 output is byte-identical
    to the original.
    """
    mb = MeshBuilder()
    # Chest plate — Z depth scales with bulk (plate thickness), X/Y silhouette fixed
    add_box(mb, center=(0, 0.25, 0), half_extents=(0.18, 0.20, 0.08 * bulk))
    # Belly section — Z depth scales with bulk
    add_box(mb, center=(0, 0.02, 0), half_extents=(0.15, 0.06, 0.07 * bulk))
    # Left pauldron — radius and height scale with bulk (thicker shoulder cap)
    add_cylinder(mb, base_center=(-0.20, 0.38, 0), radius=0.07 * bulk, height=0.06 * bulk, sides=8)
    # Right pauldron
    add_cylinder(mb, base_center=(0.20, 0.38, 0), radius=0.07 * bulk, height=0.06 * bulk, sides=8)
    # Belt — Z depth scales with bulk
    add_box(mb, center=(0, -0.04, 0), half_extents=(0.17, 0.02, 0.09 * bulk))
    # Belt buckle — kept fixed (decorative detail, not thickness-related)
    add_box(mb, center=(0, -0.04, -0.10), half_extents=(0.03, 0.02, 0.01))
    return mb

def gen_boots(height=0.2, bulk=1.0):
    """Boots — a pair of armored boots with shin guards.

    bulk scales the X width and Z depth of each section (LIGHT<1.0 = slim cloth boot,
    HEAVY>1.0 = thick plated greave with chunky sole).  Y heights (shin length, foot
    height) are preserved so the boot stays properly proportioned.  At bulk=1.0 the
    output is byte-identical to the original.
    """
    mb = MeshBuilder()
    # Left boot — shin (X width + Z depth scale with bulk)
    add_box(mb, center=(-0.06, 0.12, 0), half_extents=(0.04 * bulk, 0.10, 0.04 * bulk))
    # Left boot — foot (X width + Z depth scale with bulk)
    add_box(mb, center=(-0.06, 0.02, -0.03), half_extents=(0.04 * bulk, 0.02, 0.07 * bulk))
    # Left boot — toe cap (scales with bulk for matching thickness)
    add_box(mb, center=(-0.06, 0.03, -0.11), half_extents=(0.035 * bulk, 0.025, 0.02 * bulk))
    # Left boot — heel (scales with bulk)
    add_box(mb, center=(-0.06, 0.01, 0.04), half_extents=(0.03 * bulk, 0.01, 0.02 * bulk))
    # Right boot — shin
    add_box(mb, center=(0.06, 0.12, 0), half_extents=(0.04 * bulk, 0.10, 0.04 * bulk))
    # Right boot — foot
    add_box(mb, center=(0.06, 0.02, -0.03), half_extents=(0.04 * bulk, 0.02, 0.07 * bulk))
    # Right boot — toe cap
    add_box(mb, center=(0.06, 0.03, -0.11), half_extents=(0.035 * bulk, 0.025, 0.02 * bulk))
    # Right boot — heel
    add_box(mb, center=(0.06, 0.01, 0.04), half_extents=(0.03 * bulk, 0.01, 0.02 * bulk))
    return mb

def gen_gloves(height=0.16, bulk=1.0):
    """Gloves — a detailed pair of armored gauntlets standing UPRIGHT (fingers pointing up).

    Each hand, bottom→top: a flared cuff, a back-of-hand/palm plate (tall in Y, thin in Z so
    the palm faces -Z), a raised knuckle guard, four segmented fingers pointing UP (+Y), and an
    angled two-part thumb on the outboard side. Hands sit at ±0.06 X. The vertical fingers-up
    pose makes dropped gloves read distinctly from the toes-forward boots mesh as ground loot.

    bulk scales cuff width/depth and palm plate Z thickness (LIGHT<1.0 = slim cloth glove,
    HEAVY>1.0 = thick plated gauntlet with wider cuffs).  Finger/knuckle Y lengths are
    preserved.  At bulk=1.0 the output is byte-identical to the original.
    """
    mb = MeshBuilder()
    finger_dx = (-0.024, -0.008, 0.008, 0.024)  # four fingers across the hand width
    for sx in (-1.0, 1.0):
        x = 0.06 * sx
        # Flared cuff — X width and Z depth scale with bulk (thicker rim for heavy gauntlet)
        add_box(mb, center=(x, 0.005, 0.0), half_extents=(0.050 * bulk, 0.011, 0.030 * bulk))
        add_box(mb, center=(x, 0.030, 0.0), half_extents=(0.044 * bulk, 0.020, 0.026 * bulk))
        # Back-of-hand / palm plate — Z thickness scales with bulk; Y height kept fixed
        add_box(mb, center=(x, 0.078, 0.0), half_extents=(0.036, 0.034, 0.018 * bulk))
        # Raised knuckle guard — Z depth scales with bulk
        add_box(mb, center=(x, 0.112, -0.004), half_extents=(0.034, 0.011, 0.020 * bulk))
        # Four segmented fingers — Z depth scales with bulk; Y length and X spacing fixed
        for dx in finger_dx:
            fx = x + dx
            add_box(mb, center=(fx, 0.138, -0.002), half_extents=(0.0075, 0.022, 0.013 * bulk))
            add_box(mb, center=(fx, 0.168, -0.002), half_extents=(0.0065, 0.014, 0.011 * bulk))
        # Two-part thumb — Z depth scales with bulk
        add_box(mb, center=(x + 0.040 * sx, 0.088, 0.004), half_extents=(0.011, 0.018, 0.012 * bulk))
        add_box(mb, center=(x + 0.050 * sx, 0.110, 0.004), half_extents=(0.009, 0.014, 0.010 * bulk))
    return mb

def gen_ring(radius=0.05):
    """Ring — torus band with a single gemstone facing outward (-Z)."""
    mb = MeshBuilder()
    # Torus ring band lying flat in XZ plane
    add_torus(mb, center=(0, 0, 0), major_r=0.04, minor_r=0.012,
              major_segs=12, minor_segs=6)
    # Prong setting at front of ring
    add_box(mb, center=(0, 0, -0.052), half_extents=(0.01, 0.008, 0.006))
    # Gemstone — diamond shape facing outward (two boxes rotated 45 deg for facets)
    add_box(mb, center=(0, 0, -0.07), half_extents=(0.012, 0.012, 0.01))
    add_box(mb, center=(0, 0, -0.07), half_extents=(0.008, 0.016, 0.008))
    return mb

def gen_shield(width=0.3):
    """Shield — kite shield shape with rim, boss, and strap."""
    mb = MeshBuilder()
    # Main shield face (tall kite shape approximated as tapered boxes)
    add_box(mb, center=(0, 0.25, 0), half_extents=(0.14, 0.15, 0.012))
    # Lower taper
    add_box(mb, center=(0, 0.05, 0), half_extents=(0.10, 0.10, 0.012))
    # Rim band (top)
    add_box(mb, center=(0, 0.40, 0), half_extents=(0.12, 0.01, 0.018))
    # Rim band (sides)
    add_box(mb, center=(-0.14, 0.25, 0), half_extents=(0.01, 0.14, 0.018))
    add_box(mb, center=(0.14, 0.25, 0), half_extents=(0.01, 0.14, 0.018))
    # Center boss (raised dome)
    add_cylinder(mb, base_center=(0, 0.20, -0.012), radius=0.04, height=0.025, sides=8)
    # Back strap (arm grip)
    add_box(mb, center=(0, 0.25, 0.02), half_extents=(0.03, 0.08, 0.01))
    return mb

def gen_mace(length=0.6):
    """Mace — shaft with flanged spiked head."""
    mb = MeshBuilder()
    # Shaft (round)
    add_cylinder(mb, base_center=(0, -0.25, 0), radius=0.015, height=0.35, sides=6)
    # Grip wrapping (slightly wider section at bottom)
    add_cylinder(mb, base_center=(0, -0.25, 0), radius=0.02, height=0.10, sides=6)
    # Head — main mass
    add_cylinder(mb, base_center=(0, 0.10, 0), radius=0.05, height=0.08, sides=8)
    # Flanges (4 protruding blades around the head)
    for i in range(4):
        angle = math.pi * 0.5 * i
        dx = math.cos(angle) * 0.04
        dz = math.sin(angle) * 0.04
        add_box(mb, center=(dx, 0.14, dz), half_extents=(0.01, 0.03, 0.01))
    # Pommel
    add_cylinder(mb, base_center=(0, -0.27, 0), radius=0.025, height=0.02, sides=6)
    return mb

def gen_cleaver(length=0.5):
    """Cleaver — Butcher's wide heavy blade."""
    mb = MeshBuilder()
    # Wide blade (tall, thin)
    add_box(mb, center=(0.04, 0.1, 0), half_extents=(0.10, 0.14, 0.008))
    # Blade spine (thicker top edge)
    add_box(mb, center=(0.04, 0.24, 0), half_extents=(0.08, 0.01, 0.012))
    # Handle
    add_box(mb, center=(0, -0.12, 0), half_extents=(0.018, 0.10, 0.018))
    # Rivets on handle (two small boxes)
    add_box(mb, center=(0, -0.06, -0.02), half_extents=(0.005, 0.005, 0.005))
    add_box(mb, center=(0, -0.16, -0.02), half_extents=(0.005, 0.005, 0.005))
    return mb

def gen_iron_maiden(height=2.0):
    """Iron maiden — upright sarcophagus with hinged door and spikes."""
    mb = MeshBuilder()
    # Main body (coffin shape — wider at shoulders, tapered at feet)
    add_box(mb, center=(0, 1.0, 0), half_extents=(0.28, 0.85, 0.22))
    # Narrower base
    add_box(mb, center=(0, 0.08, 0), half_extents=(0.20, 0.08, 0.20))
    # Door (slightly offset forward)
    add_box(mb, center=(0, 1.0, -0.24), half_extents=(0.24, 0.78, 0.015))
    # Hinge brackets
    add_box(mb, center=(-0.26, 1.4, -0.22), half_extents=(0.02, 0.04, 0.02))
    add_box(mb, center=(-0.26, 0.6, -0.22), half_extents=(0.02, 0.04, 0.02))
    # Interior spikes (visible when door ajar)
    for sy in [0.6, 0.8, 1.0, 1.2, 1.4]:
        for sx in [-0.08, 0.0, 0.08]:
            add_box(mb, center=(sx, sy, -0.20), half_extents=(0.008, 0.008, 0.03))
    return mb

def gen_arrow(length=0.5):
    """Arrow — wood shaft, steel tip, white fletching. Three usemtl groups so the parts
    read as distinct colors in flight (far easier to see than a flat grey arrow)."""
    mb = MeshBuilder()
    # Shaft (thin cylinder + box along -Z) — wood
    mb.set_material("prop_wood")
    add_cylinder(mb, base_center=(0, 0, -0.2), radius=0.005, height=0.005, sides=4)
    add_box(mb, center=(0, 0, 0), half_extents=(0.004, 0.004, 0.20))
    # Arrowhead (diamond-shaped point) — steel
    mb.set_material("prop_iron")
    add_box(mb, center=(0, 0, -0.24), half_extents=(0.015, 0.003, 0.035))
    add_box(mb, center=(0, 0, -0.24), half_extents=(0.003, 0.015, 0.035))
    # Fletching (3 thin fins at the back) — white feathers
    mb.set_material("white")
    for i in range(3):
        angle = 2.0 * math.pi * i / 3
        dx = math.cos(angle) * 0.012
        dy = math.sin(angle) * 0.012
        add_box(mb, center=(dx, dy, 0.17), half_extents=(0.001, 0.01, 0.03))
    return mb

def gen_bolt(length=0.35):
    """Crossbow bolt — wood shaft, steel head, white vanes. Three usemtl groups for the
    same per-part coloring/visibility as the arrow."""
    mb = MeshBuilder()
    # Shaft (thicker than arrow) — wood
    mb.set_material("prop_wood")
    add_box(mb, center=(0, 0, 0), half_extents=(0.006, 0.006, 0.14))
    # Broad head (flat diamond tip) — steel
    mb.set_material("prop_iron")
    add_box(mb, center=(0, 0, -0.17), half_extents=(0.018, 0.004, 0.03))
    add_box(mb, center=(0, 0, -0.17), half_extents=(0.004, 0.018, 0.03))
    # Vanes (2 flat fins at back) — white
    mb.set_material("white")
    add_box(mb, center=(0.01, 0, 0.12), half_extents=(0.001, 0.008, 0.02))
    add_box(mb, center=(-0.01, 0, 0.12), half_extents=(0.001, 0.008, 0.02))
    return mb


# ---------------------------------------------------------------------------
# Boss mesh generators (floor-milestone encounters)
# ---------------------------------------------------------------------------

def gen_lich(height=2.0):
    """Lich sorcerer boss — hooded skeletal undead with flared robe base.

    Peaked hood with a shadowed skull cavity (front voxels discarded to create
    depth), bone-spike crown on the hood ridge, hunched bony shoulders, one
    arm raised as if gripping a staff. No legs — robe flares into a wide skirt
    cone that reaches the floor. Grid: ~9w x 18h.
    """
    mb = MeshBuilder()
    vs = height / 18.0   # 18 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Peaked hood (deep with shadow cavity) ---
    fill_box(-2, 13, -2, 5, 5, 4)   # main hood block
    fill_box(-1, 17, -1, 3, 1, 3)   # hood peak
    # Carve a deep shadow cavity in the front — skull sits inside
    for gy in range(14, 17):
        filled.discard((-1, gy, -2))
        filled.discard((0,  gy, -2))
        filled.discard((1,  gy, -2))
    # Small skull set back inside the cavity
    fill_box(-1, 14, -1, 3, 2, 2)
    # Eye sockets on the skull (glowing through the shadow)
    filled.discard((-1, 15, -1))
    filled.discard((1,  15, -1))

    # --- Crown spikes on hood ridge ---
    filled.add((-2, 18, 0))   # left crown spike
    filled.add(( 2, 18, 0))   # right crown spike
    filled.add(( 0, 18, 0))   # center crown spike (tallest)
    filled.add(( 0, 19, 0))   # center spike tip

    # --- Hunched bony neck/shoulders ---
    fill_box(-1, 12, 0, 3, 1, 1)    # neck
    fill_box(-3, 11, -1, 7, 2, 3)   # wide-set hunched shoulders
    # Shoulder bone protrusions
    filled.add((-4, 12, 0))
    filled.add(( 4, 12, 0))

    # --- Torso (slim, robed) ---
    fill_box(-2, 7, -1, 5, 4, 3)

    # --- Left arm (hanging, bony) ---
    fill_box(-3, 9, 0, 1, 3, 1)   # left upper arm
    fill_box(-3, 6, 0, 1, 3, 1)   # left lower arm
    fill_box(-4, 5, -1, 2, 1, 2)  # left clawed hand

    # --- Right arm (raised, gripping staff) ---
    fill_box(3, 8, 0, 1, 4, 1)    # right upper arm raised
    fill_box(3, 12, 0, 1, 2, 1)   # right upper arm connects to shoulder
    fill_box(3, 5, 0, 1, 3, 1)    # right lower arm (held lower)
    fill_box(3, 4, -1, 1, 1, 2)   # right hand / grip

    # --- Floor-length robe skirt (cone widening toward base) ---
    # Robe flares: each lower row is wider — no legs visible
    fill_box(-2, 4, -1, 5, 3, 3)   # upper skirt (narrow)
    fill_box(-3, 2, -2, 7, 2, 4)   # mid skirt (wider)
    fill_box(-4, 0, -2, 9, 2, 4)   # base skirt (widest, floor level)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_warden(height=2.4):
    """Armored skeletal tomb-warden — broad, imposing gravestone-slab pauldrons.

    Gaunt skull under an asymmetric broken crown (one spike discarded to show
    battle damage). Huge flat gravestone pauldrons on both shoulders. Armored
    cuirass over a partly-open ribcage. Two thick greaved legs. Widest-shouldered
    humanoid silhouette. Grid: ~11w x 20h.
    """
    mb = MeshBuilder()
    vs = height / 20.0   # 20 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Skull (gaunt, angular) ---
    fill_box(-2, 16, -2, 5, 4, 4)
    # Jaw
    fill_box(-1, 15, -2, 3, 1, 3)
    # Eye sockets
    filled.discard((-1, 18, -2))
    filled.discard(( 1, 18, -2))
    # Broken asymmetric crown — left spike intact, right spike broken
    fill_box(-2, 20, -1, 1, 2, 1)   # left full spike
    fill_box(-1, 20, -1, 1, 1, 1)   # center stub (short)
    fill_box( 1, 20, -1, 1, 2, 1)   # right spike intact
    # Right crown spike tip is discarded (broken)
    filled.discard((1, 21, -1))

    # --- Thick neck ---
    fill_box(-1, 14, -1, 3, 2, 2)

    # --- Gravestone-slab pauldrons (huge flat slabs, iconically wide) ---
    fill_box(-5, 12, -1, 2, 4, 3)   # left pauldron slab
    fill_box( 4, 12, -1, 2, 4, 3)   # right pauldron slab
    # Pauldron edge caps
    filled.add((-6, 13, 0)); filled.add((-6, 14, 0))
    filled.add(( 6, 13, 0)); filled.add(( 6, 14, 0))

    # --- Armored cuirass over ribcage ---
    fill_box(-3, 8, -2, 7, 6, 4)    # main torso/cuirass
    # Ribcage gaps through front of cuirass (skeletal showing through)
    for ry in [9, 11]:
        filled.discard((-1, ry, -2))
        filled.discard(( 1, ry, -2))

    # --- Belt / waist ---
    fill_box(-2, 7, -1, 5, 1, 3)

    # --- Heavy gauntlet arms ---
    fill_box(-4, 9, 0, 1, 4, 1)    # left upper arm
    fill_box( 4, 9, 0, 1, 4, 1)    # right upper arm
    fill_box(-4, 5, 0, 1, 4, 1)    # left lower arm
    fill_box( 4, 5, 0, 1, 4, 1)    # right lower arm
    # Big gauntlet fists
    fill_box(-5, 4, -1, 2, 2, 2)
    fill_box( 4, 4, -1, 2, 2, 2)

    # --- Armored greaves (thick plate legs) ---
    fill_box(-3, 3, -1, 3, 4, 3)   # left thigh greave
    fill_box( 1, 3, -1, 3, 4, 3)   # right thigh greave
    fill_box(-3, 0, -1, 3, 3, 3)   # left shin greave
    fill_box( 1, 0, -1, 3, 3, 3)   # right shin greave
    # Sabatons (armored boots, flat-bottomed)
    fill_box(-3, 0, -2, 3, 1, 4)
    fill_box( 1, 0, -2, 3, 1, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_spider_queen(radius=0.7):
    """Spider Queen boss — massively bloated egg-sac abdomen, crown of eye voxels.

    ~40% larger than gen_spider. Huge rounded abdomen at rear, smaller
    cephalothorax up front. Front crown of eye voxels with carved sockets.
    8 thick legs baked in — front pair raised higher than regular spider.
    Grid follows horizontal spider convention: x-span wide x y-span tall.
    """
    mb = MeshBuilder()
    vs = radius / 7.0   # 7 voxels per radius gives a bigger grid than spider's 5
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Bloated egg-sac abdomen (rear, large rounded block) ---
    fill_box(-4, 1, 2, 8, 6, 7)   # main abdomen
    # Round the corners of the abdomen
    for corner in [(-4,6,2),(-4,6,8),(3,6,2),(3,6,8),
                   (-4,1,2),(-4,1,8),(3,1,2),(3,1,8)]:
        filled.discard(corner)
    fill_box(-3, 7, 3, 6, 1, 5)   # abdomen dome top

    # --- Cephalothorax (front, smaller) ---
    fill_box(-3, 2, -4, 6, 4, 5)
    # Narrow neck between thorax and abdomen
    fill_box(-2, 3, 1, 4, 2, 2)

    # --- Head with crown of eyes ---
    fill_box(-2, 2, -7, 4, 3, 3)   # main head
    # Eye crown cluster — small voxels on top of head
    for ex, ez in [(-2, -6), (-1, -7), (0, -7), (1, -7), (2, -6)]:
        filled.add((ex, 5, ez))
    # Carve eye sockets in the crown
    filled.discard((-1, 5, -7))
    filled.discard(( 1, 5, -7))
    # Front face eyes (two pairs)
    filled.discard((-1, 4, -7))
    filled.discard(( 1, 4, -7))
    # Fangs below head
    filled.add((-1, 1, -8)); filled.add((1, 1, -8))
    filled.add((-1, 2, -8)); filled.add((1, 2, -8))

    # --- 8 thick legs (longer/taller than regular spider) ---
    # Front 2 pairs — raised higher
    for sz in [-3, -2]:
        filled.add((-3, 3, sz)); filled.add((-4, 4, sz)); filled.add((-5, 6, sz))
        filled.add((-6, 5, sz)); filled.add((-7, 4, sz)); filled.add((-8, 3, sz))
        filled.add((-8, 1, sz)); filled.add((-9, 0, sz))
        filled.add(( 2, 3, sz)); filled.add(( 3, 4, sz)); filled.add(( 4, 6, sz))
        filled.add(( 5, 5, sz)); filled.add(( 6, 4, sz)); filled.add(( 7, 3, sz))
        filled.add(( 7, 1, sz)); filled.add(( 8, 0, sz))
    # Rear 2 pairs — lower
    for sz in [0, 1]:
        filled.add((-3, 2, sz)); filled.add((-4, 3, sz)); filled.add((-5, 5, sz))
        filled.add((-6, 4, sz)); filled.add((-6, 3, sz)); filled.add((-7, 2, sz))
        filled.add((-8, 0, sz))
        filled.add(( 2, 2, sz)); filled.add(( 3, 3, sz)); filled.add(( 4, 5, sz))
        filled.add(( 5, 4, sz)); filled.add(( 5, 3, sz)); filled.add(( 6, 2, sz))
        filled.add(( 7, 0, sz))

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_korvath(height=2.4):
    """Korvath — armored siege juggernaut. Broadest silhouette of all bosses.

    Massive horned great-helm with a visor slit (carved eye slot). Enormous
    blocky pauldrons. A tower shield baked onto the left arm as a large flat
    slab. Spiked gauntlet right fist. Heavy plate cuirass + faulds. Grid: ~13w x 20h.
    """
    mb = MeshBuilder()
    vs = height / 20.0   # 20 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Great-helm with horns ---
    fill_box(-3, 15, -3, 7, 5, 5)   # main helm block
    # Visor slit — horizontal carved eye slot
    for vx in range(-2, 4):
        filled.discard((vx, 17, -3))
    # Horns sweeping up and outward
    fill_box(-5, 18, -1, 2, 3, 1)   # left horn
    fill_box(-6, 20, -1, 1, 1, 1)   # left horn tip
    fill_box( 4, 18, -1, 2, 3, 1)   # right horn
    fill_box( 5, 20, -1, 1, 1, 1)   # right horn tip (slightly taller)
    filled.add(( 5, 21, -1))         # right horn extra tip

    # --- Thick neck ---
    fill_box(-2, 13, -1, 5, 2, 3)

    # --- Enormous blocky pauldrons ---
    fill_box(-6, 11, -2, 3, 4, 4)   # left pauldron (wide block)
    fill_box( 4, 11, -2, 3, 4, 4)   # right pauldron
    # Pauldron spikes
    filled.add((-7, 14, 0)); filled.add((-7, 15, 0))
    filled.add(( 6, 14, 0)); filled.add(( 6, 15, 0))

    # --- Heavy plate cuirass + faulds ---
    fill_box(-3, 7, -2, 7, 6, 4)    # cuirass
    fill_box(-4, 6, -2, 9, 1, 4)    # fauld/hip plate (slightly wider)
    fill_box(-3, 5, -2, 7, 1, 3)    # lower fauld

    # --- Tower shield baked onto LEFT arm (large flat slab) ---
    fill_box(-7, 4, -3, 2, 9, 1)    # shield face (flat, tall slab)
    fill_box(-7, 4, -2, 2, 9, 1)    # shield depth
    fill_box(-5, 8, 0, 1, 5, 1)     # left upper arm behind shield
    fill_box(-5, 4, 0, 1, 4, 1)     # left lower arm

    # --- Right arm (spiked gauntlet fist) ---
    fill_box( 4, 8, 0, 1, 5, 1)     # right upper arm
    fill_box( 4, 4, 0, 1, 4, 1)     # right lower arm
    # Spiked gauntlet
    fill_box( 4, 3, -2, 2, 2, 3)    # right fist
    filled.add(( 5, 4, -3))          # spike front
    filled.add(( 4, 5, -3))          # spike front 2

    # --- Thick armored legs ---
    fill_box(-4, 2, -1, 4, 3, 3)    # left leg
    fill_box( 1, 2, -1, 4, 3, 3)    # right leg
    fill_box(-4, 0, -1, 4, 2, 3)    # left lower leg
    fill_box( 1, 0, -1, 4, 2, 3)    # right lower leg
    # Sabatons (broad, armored)
    fill_box(-4, 0, -2, 4, 1, 4)
    fill_box( 1, 0, -2, 4, 1, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_azhar(height=2.2):
    """Azhar — lean demon duelist. Deliberately slender, contrasts with bulky butcher.

    Narrow waist, long swept-back curved horns (extend several voxels back and
    up), angular ashen face, flared shoulder spikes. Right arm extended holding
    a long thin blade. Thin tattered cape baked on the back. Clawed digit legs.
    Grid: ~9w x 20h (narrow silhouette).
    """
    mb = MeshBuilder()
    vs = height / 20.0   # 20 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Angular head ---
    fill_box(-2, 16, -2, 5, 4, 4)
    # Narrow angular jaw
    fill_box(-1, 15, -2, 3, 1, 3)
    # Eye sockets (angular slits)
    filled.discard((-1, 18, -2))
    filled.discard(( 1, 18, -2))

    # --- Long swept-back curved horns ---
    # Horns curve back and up from the skull sides
    fill_box(-3, 19, -1, 1, 1, 1)   # left horn base
    filled.add((-3, 20, 0))           # left horn mid
    filled.add((-3, 21, 1))           # left horn upper sweep
    filled.add((-2, 22, 2))           # left horn tip (sweeps back)
    fill_box( 2, 19, -1, 1, 1, 1)   # right horn base
    filled.add(( 2, 20, 0))           # right horn mid
    filled.add(( 2, 21, 1))           # right horn upper sweep
    filled.add(( 1, 22, 2))           # right horn tip

    # --- Neck (slender) ---
    fill_box(-1, 14, -1, 3, 2, 2)

    # --- Narrow torso with flared shoulder spikes ---
    fill_box(-2, 10, -1, 5, 4, 3)   # torso (narrow)
    # Shoulder spikes (angular, flared)
    filled.add((-3, 13, 0)); filled.add((-4, 14, 0))   # left spike
    filled.add(( 3, 13, 0)); filled.add(( 4, 14, 0))   # right spike

    # --- Narrow waist ---
    fill_box(-1, 8, -1, 3, 2, 2)

    # --- Thin tattered cape (back slab, behind torso) ---
    fill_box(-2, 5, 2, 5, 9, 1)   # cape: runs from hips up to shoulder height

    # --- Left arm (hanging, clawed) ---
    fill_box(-3, 10, 0, 1, 4, 1)   # left upper arm
    fill_box(-3, 6, 0, 1, 4, 1)    # left lower arm
    fill_box(-4, 5, -1, 2, 1, 2)   # left clawed hand

    # --- Right arm extended holding a long thin blade ---
    fill_box( 3, 10, 0, 1, 4, 1)   # right upper arm
    fill_box( 3, 6, 0, 1, 4, 1)    # right lower arm
    fill_box( 3, 4, -1, 1, 2, 1)   # right hand/grip
    # Long thin blade extending from the hand upward
    fill_box( 4, 5, -1, 1, 8, 1)   # blade (tall, thin column)
    filled.add(( 4, 13, -1))         # blade tip

    # --- Narrow pelvis ---
    fill_box(-2, 7, -1, 5, 1, 2)

    # --- Clawed digit legs (slender, digitigrade) ---
    fill_box(-2, 3, -1, 2, 4, 2)   # left thigh
    fill_box( 1, 3, -1, 2, 4, 2)   # right thigh
    fill_box(-2, 1, -1, 2, 2, 1)   # left lower leg (slanted)
    fill_box( 1, 1, -1, 2, 2, 1)   # right lower leg
    # Clawed feet (digitigrade — heel raised)
    fill_box(-2, 0, -2, 2, 1, 2)
    fill_box( 1, 0, -2, 2, 1, 2)
    filled.add((-3, 0, -1)); filled.add(( 2, 0, -1))  # claw tips

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_diabro(height=2.6):
    """DiaBRO — iconic demon terror. Distinct hunched bestial posture.

    Long curving ram/bull horns sweeping back and up (multi-voxel curve).
    Elongated bestial maw (carved mouth). Hunched powerful torso leaning
    forward. A ridged spine row of spikes up the back. Broad clawed hands.
    Digitigrade clawed legs. Largest non-final boss. Grid: ~13w x 22h.
    """
    mb = MeshBuilder()
    vs = height / 22.0   # 22 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Massive bestial head ---
    fill_box(-3, 18, -4, 7, 4, 5)   # skull
    # Elongated bestial maw (lower jaw protrudes)
    fill_box(-2, 16, -5, 5, 2, 3)   # maw/snout
    fill_box(-2, 15, -5, 5, 1, 2)   # lower jaw
    # Carved mouth gap
    for mx in range(-2, 3):
        filled.discard((mx, 16, -5))
        filled.discard((mx, 15, -5))
    # Eyes — sunken orange embers
    filled.discard((-2, 20, -4))
    filled.discard(( 2, 20, -4))
    filled.discard((-1, 20, -4))
    filled.discard(( 1, 20, -4))

    # --- Long curving ram horns ---
    # Left horn: sweeps up and back in a curve
    fill_box(-4, 21, -2, 1, 2, 1)   # left horn base
    filled.add((-5, 23, -1))          # left horn mid sweep
    filled.add((-5, 24, 0))           # left horn upper
    filled.add((-4, 25, 1))           # left horn tip curving back
    # Right horn: mirror
    fill_box( 3, 21, -2, 1, 2, 1)
    filled.add(( 4, 23, -1))
    filled.add(( 4, 24, 0))
    filled.add(( 3, 25, 1))

    # --- Neck (thick, hunched forward) ---
    fill_box(-2, 16, -1, 5, 2, 3)   # neck leans forward

    # --- Hunched powerful torso ---
    fill_box(-4, 10, -2, 9, 6, 5)   # main torso
    # Barrel chest protrusion (hunched forward lean)
    fill_box(-3, 12, -4, 7, 3, 2)
    # Ridged spine — row of spikes up the back
    for sy in range(9, 18, 2):
        filled.add((0, sy, 3))    # spine ridge spike

    # --- Belt / waist ---
    fill_box(-3, 8, -2, 7, 2, 4)

    # --- Broad clawed hands ---
    fill_box(-6, 7, -1, 2, 5, 2)    # left upper arm
    fill_box( 5, 7, -1, 2, 5, 2)    # right upper arm
    fill_box(-6, 3, -1, 2, 4, 2)    # left lower arm
    fill_box( 5, 3, -1, 2, 4, 2)    # right lower arm
    # Broad 3-voxel-wide clawed hands
    fill_box(-7, 2, -2, 3, 2, 3)
    fill_box( 5, 2, -2, 3, 2, 3)

    # --- Digitigrade clawed legs ---
    fill_box(-4, 4, -1, 3, 4, 3)    # left thigh
    fill_box( 2, 4, -1, 3, 4, 3)    # right thigh
    fill_box(-4, 1, 0, 3, 3, 2)     # left lower leg (angled back)
    fill_box( 2, 1, 0, 3, 3, 2)     # right lower leg
    # Clawed feet pointing forward
    fill_box(-4, 0, -3, 3, 1, 4)
    fill_box( 2, 0, -3, 3, 1, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_nyx(height=2.2):
    """Nyx — void weaver. Elongated spindly torso, void-crystal crown of spikes.

    No legs — trails a void-robe base with a few tapering tendrils reaching
    the ground (wraith-like robe silhouette). Narrow hooded head.
    Grid: ~9w x 20h.
    """
    mb = MeshBuilder()
    vs = height / 20.0   # 20 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Narrow hooded head ---
    fill_box(-2, 15, -2, 5, 5, 4)
    # Hood peak (tall, narrow)
    fill_box(-1, 19, -1, 3, 2, 2)
    filled.add(( 0, 21, 0))   # hood apex voxel

    # --- Void-crystal crown (cluster of sharp spikes on the hood) ---
    filled.add((-2, 20, 0))    # left outer spike
    filled.add((-1, 21, 0))    # left inner spike
    filled.add(( 1, 21, 0))    # right inner spike (already implied by hood)
    filled.add(( 2, 20, 0))    # right outer spike
    filled.add(( 0, 22, 0))    # tallest center crystal spike
    filled.add((-1, 22, -1))   # forward left crystal
    filled.add(( 1, 22, -1))   # forward right crystal

    # --- Face cavity (carved shadow) ---
    for fy in range(16, 19):
        filled.discard((0, fy, -2))
    # Eyes inside the cavity
    fill_box(-1, 17, -1, 3, 1, 1)   # eye row
    filled.discard(( 0, 17, -1))      # center of eye row stays dark

    # --- Neck (spindly) ---
    fill_box( 0, 14, 0, 1, 1, 1)

    # --- Elongated spindly torso ---
    fill_box(-2, 9, -1, 5, 5, 3)    # upper torso
    fill_box(-1, 5, -1, 3, 4, 2)    # lower torso (narrower)

    # --- Long thin arms ---
    fill_box(-3, 11, 0, 1, 4, 1)    # left upper arm
    fill_box(-4, 7, 0, 1, 4, 1)     # left lower arm (angled)
    fill_box(-4, 5, -1, 1, 2, 1)    # left hand
    fill_box( 3, 11, 0, 1, 4, 1)    # right upper arm
    fill_box( 4, 7, 0, 1, 4, 1)     # right lower arm
    fill_box( 4, 5, -1, 1, 2, 1)    # right hand

    # --- Void-robe base (wraith-like tendrils, no legs) ---
    fill_box(-3, 3, -2, 7, 2, 4)    # upper robe bell
    fill_box(-4, 1, -2, 9, 2, 4)    # lower robe flare
    # Tapering tendrils reaching the floor
    filled.add((-3, 0, -1)); filled.add((-3, 0, 0))   # left tendril
    filled.add(( 3, 0, -1)); filled.add(( 3, 0, 0))   # right tendril
    filled.add(( 0, 0, -1)); filled.add(( 0, 0, 1))   # center tendrils

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_reaper(height=2.6):
    """Grim Reaper death boss — huge peaked hood with deep shadow skull cavity.

    Broad cloaked shoulders. Skeletal arms, one gripping a tall SCYTHE baked
    in (a pole with an angled blade at the top). No legs — cloak flares to the
    floor with a tattered hem. Grid: ~13w x 22h.
    """
    mb = MeshBuilder()
    vs = height / 22.0   # 22 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Huge peaked hood ---
    fill_box(-3, 17, -3, 7, 5, 5)   # main hood block
    fill_box(-2, 21, -2, 5, 2, 4)   # hood peak upper
    fill_box(-1, 23, -1, 3, 1, 2)   # hood apex
    filled.add(( 0, 24, 0))           # top of hood

    # --- Deep shadow skull cavity (carve front face) ---
    for gy in range(18, 21):
        for gx in range(-2, 3):
            filled.discard((gx, gy, -3))
    # Small skull deep inside the hood
    fill_box(-1, 18, -2, 3, 2, 2)
    # Skull eye sockets (glowing)
    filled.discard((-1, 19, -2))
    filled.discard(( 1, 19, -2))

    # --- Broad cloaked shoulders ---
    fill_box(-5, 14, -2, 11, 3, 4)   # wide shoulder drape
    # Shoulder edge definition
    fill_box(-6, 15, -1, 1, 2, 2)
    fill_box( 6, 15, -1, 1, 2, 2)

    # --- Neck ---
    fill_box(-1, 16, -1, 3, 1, 2)

    # --- Cloaked torso (slim under cloak) ---
    fill_box(-3, 10, -1, 7, 4, 3)

    # --- Skeletal left arm (hanging, bony) ---
    fill_box(-4, 11, 0, 1, 4, 1)    # left upper arm
    fill_box(-4, 7, 0, 1, 4, 1)     # left lower arm
    fill_box(-5, 6, -1, 2, 1, 2)    # left bony hand

    # --- Right arm gripping a tall scythe ---
    fill_box( 4, 11, 0, 1, 4, 1)    # right upper arm
    fill_box( 4, 7, 0, 1, 4, 1)     # right lower arm
    fill_box( 4, 5, 0, 1, 2, 1)     # right hand/grip
    # Scythe pole (tall, extends high above the head)
    fill_box( 5, 1, 0, 1, 22, 1)    # scythe pole (tall thin column)
    # Scythe blade (angled at the top, curving left)
    fill_box( 2, 22, -1, 4, 1, 1)   # blade horizontal span
    fill_box( 2, 21, -2, 3, 1, 1)   # blade lower angled edge
    filled.add(( 1, 22, -1))          # blade tip left

    # --- Cloak base (no legs, flares to floor with tattered hem) ---
    fill_box(-4, 7, -2, 9, 3, 4)    # upper cloak bell
    fill_box(-5, 4, -2, 11, 3, 4)   # mid cloak flare
    fill_box(-6, 1, -2, 13, 3, 4)   # lower cloak (widest)
    # Tattered hem tendrils
    for tx in [-5, -3, -1, 1, 3, 5]:
        filled.add((tx, 0, -1))

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_player_warrior(height=1.8):
    """Player Warrior class — heavy plate veteran with crimson sash + cape.

    Stocky / hulking silhouette: widened torso, oversized rounded pauldrons,
    full great-helm with a single horizontal eye-slit, short cape draped
    behind the shoulders. Reads as a wall rather than as a tall figure.
    Origin at feet (Y=0).
    """
    mb = MeshBuilder()
    # 17 voxels tall — one extra row over the standard 16-voxel humanoid so
    # the helm sits a bit higher and the build reads as "hulking" without
    # actually being taller than a 1.8 m human in world units.
    vs = height / 17.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Full great helm (gy=14..16) ---
    # Wide 5-voxel block, 3 tall, 4 deep — fully encloses the head, no skin.
    fill_box(-2, 14, -2, 5, 3, 4)
    # Carve the FRONT face of the middle row to make a horizontal eye-slit.
    # Only gz=-2 is discarded so the slit color shows on the helm's front
    # but the helmet shell stays solid on its sides/back — otherwise the
    # dark eye-slit pixel column would bleed onto the side faces too.
    for gx in range(-1, 2):           # gx = -1, 0, 1 — slit is 3 voxels wide
        filled.discard((gx, 15, -2))
    # Helm crest/dome — a slightly narrower row on top so the silhouette
    # doesn't read as a pure cube.
    fill_box(-1, 17, -1, 3, 0, 0)     # (no-op safety; explicit add below)
    for gx in range(-1, 2):
        filled.add((gx, 17, -1))
        filled.add((gx, 17, 0))

    # --- Neck gorget (thick, hidden under helm rim) ---
    fill_box(-1, 13, -1, 3, 1, 2)

    # --- Broad armored torso (gy=7..12) ---
    # 7 voxels wide (gx=-3..3) — broader than the 5-wide paladin chest so
    # the warrior reads "wall-like" from the front.
    fill_box(-3, 7, -1, 7, 6, 3)

    # --- Crimson sash band (gy=6) ---
    # One row across the waist — the skin texture paints this row red.
    fill_box(-3, 6, -1, 7, 1, 3)

    # --- Belt + tassets (gy=4..5) ---
    fill_box(-3, 4, -1, 7, 2, 3)

    # --- Oversized rounded pauldrons (gy=11..13) ---
    # Extend out to gx=-5/+5, 2 voxels thick. Top corners trimmed to give
    # a "rounded" feel (a flat 2x3 block reads as a brick).
    fill_box(-5, 11, -1, 2, 2, 3)     # left pauldron block
    fill_box( 4, 11, -1, 2, 2, 3)     # right pauldron block
    # Trim the outer top corners so each pauldron has a rounded shoulder line.
    filled.discard((-5, 12, -1))
    filled.discard((-5, 12,  1))
    filled.discard(( 5, 12, -1))
    filled.discard(( 5, 12,  1))
    # Pauldron top cap — single voxel ridge for a more sculpted look.
    filled.add((-4, 13, 0))
    filled.add(( 4, 13, 0))

    # --- Thick armored arms ---
    # Arms hang below the pauldron at gx=-4 / +4 so the pauldron silhouette
    # overhangs the shoulder joint (chunky look).
    fill_box(-4, 8, 0, 1, 3, 1)       # left upper arm
    fill_box( 4, 8, 0, 1, 3, 1)       # right upper arm
    fill_box(-4, 4, 0, 1, 4, 1)       # left lower arm + gauntlet upper
    fill_box( 4, 4, 0, 1, 4, 1)       # right lower arm + gauntlet upper
    # Gauntlet fists — protrude forward (gz=-1) so they read as fists.
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box( 4, 3, -1, 1, 1, 2)

    # --- Heavy armored legs ---
    fill_box(-2, 2, -1, 2, 2, 2)      # left thigh
    fill_box( 1, 2, -1, 2, 2, 2)      # right thigh
    fill_box(-2, 0, -1, 2, 2, 2)      # left greave
    fill_box( 1, 0, -1, 2, 2, 2)      # right greave
    # Sabatons — armored boots with extended toe (gz=-2).
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box( 1, 0, -2, 2, 1, 3)

    # --- Crimson cape behind the shoulders ---
    # Draped down the back from the pauldrons to below the waist. Placed at
    # gz=2 (one voxel behind the torso's back face at gz=1) so it reads as a
    # separate layer rather than just a back-coloured torso row.
    cape_voxels = []
    def add_cape(x, y, z):
        v = (x, y, z)
        filled.add(v)
        cape_voxels.append(v)

    for gy in range(7, 13):           # cape body gy=7..12 (behind pauldrons → waist)
        for gx in range(-2, 3):
            add_cape(gx, gy, 2)
    for gx in range(-1, 2):           # cape tail gy=5..6 (narrower)
        add_cape(gx, 6, 2)
        add_cape(gx, 5, 2)
    add_cape(0, 4, 2)                  # cape tip

    # Cape voxels share (gx, gy) with torso/sash/belt columns, so we must
    # remap them to a dedicated "cape" pixel — otherwise the cape would paint
    # the plate-grey torso colour. We pick (gx=5, gy=17): the gx=5 column is
    # only occupied by the pauldron at gy=11..13, and gy=17 is the helm
    # crest row (only gx=-1..1) — so this grid cell has no natural sampler.
    # uv_overrides values are in GRID space (line 237: ugx - min_gx), so
    # (5, 17) -> pixel (5 - (-5), 17) = (10, 17), the top-right texel.
    uv_overrides = {v: (5, 17) for v in cape_voxels}

    # Grid: gx in [-5, 5] (w=11), gy in [0, 17] (h=18)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs), uv_overrides=uv_overrides)
    return mb


def gen_player_paladin(height=1.85):
    """Player Paladin class — holy crusader in white-and-gold plate.

    Reads visibly more ornate than the existing `gen_paladin` NPC:
      - Winged helm (small wing-flares at ear height on each side).
      - Tall helm crest spine + flat-top crown.
      - Centered sunburst chest emblem on a gold-trimmed white tabard
        that hangs down the FRONT of the torso to mid-thigh.
      - Chunky pauldrons that overhang the shoulders.
      - Heavy plate boots that rise up the calf.
    Origin at feet (Y=0). Tall, proud silhouette — squared shoulders.
    """
    mb = MeshBuilder()
    # 17 voxels tall (matches the warrior class). vs slightly tall vs the
    # 16-voxel NPC paladin so the figure reads as a proud crusader.
    vs = height / 17.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Helm (gy=13..16) ---
    # 5-wide, 3-tall block — fully encloses the head.
    fill_box(-2, 13, -2, 5, 3, 4)
    # Visor slit at gy=14 (eye row). Discard only the FRONT face (gz=-2);
    # side and back faces stay solid so the dark slit colour doesn't bleed
    # onto the helm sides via the per-voxel UV lookup.
    for gx in range(-2, 3):
        filled.discard((gx, 14, -2))

    # --- Winged helm: small wing-flares at ear height (gy=14..15) ---
    # Two-voxel triangular wings on each side, sloping up-and-back so the
    # silhouette reads as feathered rather than as a brick. Wings sit at
    # gz=0..1 (mid/back of helm) and reach out to gx=-4 / +4.
    # Left wing: lower voxel forward, upper voxel back & slightly higher.
    filled.add((-3, 14, 0))    # inner wing root (touches helm side)
    filled.add((-4, 14, 0))    # outer wing tip lower
    filled.add((-4, 15, 1))    # outer wing tip upper-back (sloped up & back)
    # Right wing mirrored.
    filled.add(( 3, 14, 0))
    filled.add(( 4, 14, 0))
    filled.add(( 4, 15, 1))

    # --- Helm crest (gy=17): tall narrow spine on top of the crown ---
    # A 3-voxel ridge along the centerline gives the proud-crusader profile
    # and distinguishes this from the flat-top NPC paladin helm.
    for gx in range(-1, 2):
        filled.add((gx, 17, 0))
    # Single peak voxel at the front of the crest for a forward-leaning
    # plume — adds asymmetry vs the flat warrior helm cap.
    filled.add((0, 17, -1))

    # --- Neck gorget / collar (gy=12) ---
    # 3-wide, 2-deep — visible band between helm rim and chest plate.
    fill_box(-1, 12, -1, 3, 1, 2)

    # --- Squared chest plate (gy=6..11) ---
    # 5 wide (gx=-2..2), 3 deep — a "proud" rectangular torso rather than
    # a 7-wide warrior wall, so the tabard reads clearly down the centre.
    fill_box(-2, 6, -1, 5, 6, 3)

    # --- Belt / tassets (gy=4..5) ---
    fill_box(-2, 4, -1, 5, 2, 3)

    # --- Chunky pauldrons (gy=10..12) ---
    # Extend one voxel WIDER than the torso (out to gx=-5 / +5) and one
    # voxel TALLER than the warrior's so they over-hang the shoulder.
    fill_box(-5, 10, -1, 2, 3, 3)     # left pauldron
    fill_box( 4, 10, -1, 2, 3, 3)     # right pauldron
    # Round the outer top corners so the pauldrons don't read as bricks.
    filled.discard((-5, 12, -1))
    filled.discard((-5, 12,  1))
    filled.discard(( 5, 12, -1))
    filled.discard(( 5, 12,  1))
    # Pauldron ridge cap — a single highlight voxel on each shoulder top.
    filled.add((-4, 13, 0))
    filled.add(( 4, 13, 0))

    # --- Armoured arms (gy=4..9) ---
    # Arms hang one column inside the pauldrons (gx=-4 / +4) so the
    # pauldron silhouette overhangs them.
    fill_box(-4, 7, 0, 1, 3, 1)       # left upper arm
    fill_box( 4, 7, 0, 1, 3, 1)       # right upper arm
    fill_box(-4, 4, 0, 1, 3, 1)       # left forearm
    fill_box( 4, 4, 0, 1, 3, 1)       # right forearm
    # Gauntlets — protrude forward for a clenched-fist read.
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box( 4, 3, -1, 1, 1, 2)

    # --- Legs ---
    fill_box(-2, 2, -1, 2, 2, 2)      # left thigh
    fill_box( 1, 2, -1, 2, 2, 2)      # right thigh
    # Heavy plate boots that extend up the calf (gy=0..1) and one row
    # higher than the warrior sabatons via the greave-overlap below.
    fill_box(-2, 0, -1, 2, 2, 2)      # left greave/upper boot
    fill_box( 1, 0, -1, 2, 2, 2)      # right greave/upper boot
    # Sabaton toe-caps extend forward (gz=-2) for a chunky armoured foot.
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box( 1, 0, -2, 2, 1, 3)

    # --- Tabard: 3-wide vertical strip on the FRONT of the torso ---
    # Hangs from upper chest (gy=10) to mid-thigh (gy=3) at gz=-2 (one
    # voxel in front of the torso's front face at gz=-1). Tracking the
    # voxels lets us paint them with a dedicated cream-tabard pixel.
    tabard_voxels = []
    def add_tabard(x, y, z):
        v = (x, y, z)
        filled.add(v)
        tabard_voxels.append(v)
    for gy in range(3, 11):
        for gx in range(-1, 2):
            add_tabard(gx, gy, -2)

    # --- Sunburst chest emblem ---
    # Central voxel on the upper-chest of the tabard, plus a small halo
    # ring at the same row so the emblem reads from distance. The halo
    # voxels are tracked separately so the skin can paint them lighter
    # gold (a "rays" ring) while the centre uses rich gold.
    sunburst_center = (0, 10, -2)      # exists already (part of tabard)
    sunburst_halo = []
    # Halo ring: 4 voxels around centre on the same gz=-2 plane.
    for (hx, hy) in [(-1, 10), (1, 10), (0, 11), (0, 9)]:
        # gy=11 / gy=9 halo voxels — add only if not already part of tabard,
        # otherwise just track them for the UV override.
        v = (hx, hy, -2)
        if v not in filled:
            filled.add(v)
        sunburst_halo.append(v)

    # --- UV overrides ----------------------------------------------------
    # Several voxels live in (gx, gy) columns shared with plate-grey armour,
    # so we redirect their UV sample to dedicated palette pixels in unused
    # corners of the grid. The grid is gx in [-5, 5] (min=-5) and
    # gy in [0, 17]; the top corners (gx=-5/5 at gy=16/17) are empty.
    #
    # Pixel index = (gx - min_gx, gy - min_gy) = (gx + 5, gy).
    #   (5, 17)  -> pixel (10, 17): "tabard cream" colour cell
    #   (-5, 17) -> pixel (0,  17): "sunburst centre" gold cell
    #   (-5, 16) -> pixel (0,  16): "sunburst halo"   lighter gold cell
    uv_overrides = {}
    for v in tabard_voxels:
        uv_overrides[v] = (5, 17)
    # Sunburst centre takes precedence over the tabard override.
    uv_overrides[sunburst_center] = (-5, 17)
    for v in sunburst_halo:
        uv_overrides[v] = (-5, 16)

    # Back-of-head voxels at the eye row need the regular helm colour, not
    # the dark visor-slit pixel. gz=-2 at gy=14 is the visor (discarded);
    # gz=-1 is the eye behind the slit (keep). Remap gz=0 and gz=1 to a
    # neutral helm pixel just below the visor row.
    for gz in range(0, 2):
        uv_overrides[(-1, 14, gz)] = (-1, 13)
        uv_overrides[( 1, 14, gz)] = ( 1, 13)
        uv_overrides[( 0, 14, gz)] = ( 0, 13)
        uv_overrides[(-2, 14, gz)] = (-2, 13)
        uv_overrides[( 2, 14, gz)] = ( 2, 13)

    # Grid: gx in [-5, 5] (w=11), gy in [0, 17] (h=18)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs),
                    uv_overrides=uv_overrides)
    return mb


def gen_player_rogue(height=1.7):
    """Player Rogue class — shadowy knife-thrower wrapped head-to-toe in black leathers.

    Distinct from the NPC ``gen_rogue`` (which has a tall hood + back cape) by:
      * Chunkier voxels (height/15 vs height/16) — slightly shorter, more compact
        silhouette so the rogue reads as low-profile rather than towering.
      * Deep overhanging hood with a forward brim that overhangs the eye row, casting
        a hard shadow over the eyes (the brim sits one voxel ahead of the face plane).
      * Cloth face wrap covering cheeks/chin below the eyes — the hood does NOT reach
        the chin, so the wrap fabric is visible as a separate band below the brim.
      * Narrow 3-wide torso (vs the NPC rogue's 4-wide chest) with arms tucked close.
      * Two chest-strap rows for the throwing-knife bandoliers, painted by the skin
        in rust-leather brown so they pop against the near-black leathers.
      * No back cape — the rogue's back is a flat hooded shadow, not a billowing cloak.

    Origin at feet (Y=0).
    """
    mb = MeshBuilder()
    # 15-voxel tall grid -> chunkier voxels at the same world height; the in-engine
    # renderer scales characters to ~1.8 m anyway, but the chunkier per-voxel size
    # makes the silhouette read as more compact / crouched than the 16-voxel NPC.
    vs = height / 15.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (gy=11..13) ---
    # 5 wide, 3 tall, 4 deep — same head footprint as the humanoid baseline so the
    # neck/shoulders attach cleanly. gy=13 = eye row, gy=12 = cheeks (face-wrap top),
    # gy=11 = chin (covered by the cloth wrap colour).
    fill_box(-2, 11, -2, 5, 3, 4)

    # --- Deep overhanging hood (gy=13..14) ---
    # The hood crown sits over the top of the head; the side drapes wrap down past
    # the cheeks. The forward brim at gz=-3 physically overhangs the face plane
    # (gz=-2), casting a dark shadow over the eye row.
    fill_box(-2, 14, -2, 5, 1, 4)              # crown of hood
    fill_box(-3, 13, -2, 1, 2, 4)              # left side drape (extends out to gx=-3)
    fill_box( 2, 13, -2, 1, 2, 4)              # right side drape (extends out to gx= 3)
    # Forward brim — one row of voxels at gz=-3 across the top so the hood
    # overhangs the eyes. Width matches the side-drape outer extents so the brim
    # fully shadows the face from above.
    fill_box(-3, 14, -3, 7, 1, 1)              # full-width brim across the brow

    # --- Neck (gy=10) ---
    fill_box(0, 10, 0, 1, 1, 1)

    # --- Narrow torso, 3 voxels wide (gy=6..9) ---
    # Markedly narrower than the NPC rogue's 4-wide chest so the rogue reads "lean"
    # and low-profile. gy=7 and gy=8 are the two horizontal strap rows — they're
    # part of the torso block; the skin texture colours them rust-leather brown so
    # they read as the throwing-knife bandoliers crossing the chest.
    fill_box(-1, 6, -1, 3, 4, 3)               # torso main block
    # Shoulders — single ridge at gy=9 just outside the torso. Hunched/compact:
    # no separate pauldron blob, just a 1-voxel cap so the arm attaches tightly.
    filled.add((-2, 9, 0))                     # left shoulder
    filled.add(( 2, 9, 0))                     # right shoulder

    # --- Arms (close to body, 1 voxel thick) ---
    # Arms hang at gx=-2/+2 (tucked tight — narrower than the NPC rogue's gx=-3/+2)
    # so the silhouette stays compact and hunched.
    fill_box(-2, 7, 0, 1, 2, 1)                # left upper arm
    fill_box( 2, 7, 0, 1, 2, 1)                # right upper arm
    fill_box(-2, 4, 0, 1, 3, 1)                # left lower arm
    fill_box( 2, 4, 0, 1, 3, 1)                # right lower arm
    # Fingerless gloves — small forward block at the wrist (gz=-1 makes them poke
    # forward like the humanoid's hands so they read as fists/gloves not stumps).
    fill_box(-2, 3, -1, 1, 1, 2)               # left glove
    fill_box( 2, 3, -1, 1, 1, 2)               # right glove

    # --- Pelvis / belt (gy=4..5) ---
    fill_box(-1, 4, -1, 3, 2, 3)               # pelvis block (matches torso width)

    # --- Legs (gy=0..3), soft boots ---
    fill_box(-1, 2, 0, 1, 2, 1)                # left thigh
    fill_box( 1, 2, 0, 1, 2, 1)                # right thigh
    fill_box(-1, 0, 0, 1, 2, 1)                # left shin
    fill_box( 1, 0, 0, 1, 2, 1)                # right shin
    # Soft boots — extend forward by one voxel (not a chunky armoured sabaton);
    # the skin paints them slightly bluer-dark so the foot reads as a soft boot.
    fill_box(-1, 0, -1, 1, 1, 2)               # left boot toe
    fill_box( 1, 0, -1, 1, 1, 2)               # right boot toe

    # UV remap: the two eye voxels at gy=13 paint ice-blue on the FRONT face only.
    # The same (gx, gy) column on the BACK/SIDE faces of those voxels would also
    # sample the ice-blue pixel — but there's no eye opening there, so we remap
    # the rear/side voxels of the eye columns to the face-wrap pixel (gx=0, gy=12)
    # which is neutral cloth-grey. The eye voxel itself (gz=-2) stays at its native
    # pixel so the front face still glows ice-blue under the hood shadow.
    uv_fix = {}
    for gz in range(-1, 2):                    # gz=-1 (behind front face), 0, 1 (back of head)
        uv_fix[(-1, 13, gz)] = (0, 12)
        uv_fix[( 1, 13, gz)] = (0, 12)

    # Grid: gx in [-3, 3] (w=7), gy in [0, 14] (h=15)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs), uv_overrides=uv_fix)
    return mb


def gen_player_combat_engineer(height=1.85):
    """Player Combat Engineer — hazard-orange exosuit + power-pack + welder helm.

    Industrial mech-jock silhouette. Broader than the warrior: pauldrons reach
    gx=-6/+6 (w=13) versus the warrior's gx=-5/+5. Pure bulk all the way down
    (no nipped waist), oversized rounded pauldron domes, a power-pack slab on
    the upper back, boxy hardhat-style welder helm with a glowing goggle band,
    chunky boots and gauntlets. Reads as "tank in a workshop suit".
    Origin at feet (Y=0).
    """
    mb = MeshBuilder()
    # 17 voxels tall — same vertical budget as the warrior so player heights
    # match in world units, but we spend horizontal budget aggressively for
    # the hulking exosuit look.
    vs = height / 17.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Boxy welder helm (gy=14..16) ---
    # 5 wide, 3 tall, 4 deep — fully covers the head with no skin showing.
    # Hardhat-flat top instead of a knightly dome.
    fill_box(-2, 14, -2, 5, 3, 4)
    # Carve a horizontal "welder goggle" band on the FRONT face only at the
    # eye row (gy=15). Only gz=-2 voxels are discarded so the goggle accent
    # color shows on the helm front, but the side/back faces of those voxels
    # stay as helm steel (avoids the goggle color bleeding around the head).
    for gx in range(-1, 2):           # 3-voxel-wide goggle band: gx=-1,0,1
        filled.discard((gx, 15, -2))
    # Helm brim — a thin lip protruding forward over the goggles to read
    # as an industrial hardhat visor (gy=16, gz=-3 one voxel forward of helm).
    for gx in range(-2, 3):
        filled.add((gx, 16, -3))

    # --- Neck gorget / collar (gy=13) ---
    # Wider than warrior's neck so the suit reads as one continuous bulk.
    fill_box(-2, 13, -1, 5, 1, 3)

    # --- Chunky exosuit torso (gy=7..12) ---
    # 7 voxels wide (gx=-3..3), 3 deep. Same width as the warrior chest but
    # carries straight down through the hips (no waist taper) to read as
    # "tank in a suit" rather than an athletic build.
    fill_box(-3, 7, -1, 7, 6, 3)

    # --- Power-pack / backpack (gy=9..12 upper back) ---
    # 3 wide x 4 tall x 1 deep slab sitting on the upper back, sticking out
    # one voxel behind the torso (gz=2). Reads as a separate industrial unit
    # rather than torso volume. We tag these voxels for a darker gunmetal
    # palette and a cyan glow vent column via uv_overrides.
    pack_voxels = []
    def add_pack(x, y, z):
        v = (x, y, z)
        filled.add(v)
        pack_voxels.append(v)
    for gy in range(9, 13):
        for gx in range(-1, 2):
            add_pack(gx, gy, 2)
    # Two side mounting clamps on the upper edges of the pack — single voxels
    # sticking out further to imply hardware bolted to the suit.
    pack_clamps = [(-2, 11, 2), (2, 11, 2)]
    for v in pack_clamps:
        filled.add(v)
        pack_voxels.append(v)

    # --- Wide hip/belt block (gy=3..6) ---
    # Crucial for the "no narrow waist" silhouette — keep it as broad as the
    # torso all the way down to the legs. 7 wide, 4 tall.
    fill_box(-3, 3, -1, 7, 4, 3)

    # --- Oversized rounded pauldron domes (gy=11..13) ---
    # Each dome is a 2x2x2 voxel block sitting outboard of the torso, with
    # the topmost outer corners removed so the silhouette reads as a rounded
    # dome rather than a hard cube.
    # Left dome: gx=-5..-4, gy=11..12, gz=-1..0
    fill_box(-5, 11, -1, 2, 2, 2)
    # Right dome: gx=4..5
    fill_box( 4, 11, -1, 2, 2, 2)
    # Round off the outer-top corners of each dome.
    for v in [(-5, 12, -1), (-5, 12, 0),
              ( 5, 12, -1), ( 5, 12, 0)]:
        filled.discard(v)
    # Pauldron outer ridge — single voxel at gx=-6/+6 mid-height to push the
    # silhouette one voxel wider than the warrior so the engineer reads as
    # visibly bulkier than any other player class.
    filled.add((-6, 11, 0))
    filled.add(( 6, 11, 0))

    # --- Bulky armored upper arms (gy=8..10) ---
    # Hang inboard of the pauldron domes (gx=-4/+4) so the dome overhangs the
    # joint. 1 voxel wide, 3 tall.
    fill_box(-4, 8, 0, 1, 3, 1)        # left upper arm
    fill_box( 4, 8, 0, 1, 3, 1)        # right upper arm

    # --- Bulky gauntlets / forearms (gy=4..7) ---
    # 2 voxels wide (one wider than the upper arm) to give the "gauntlet
    # flares wider than the bicep" industrial look. The outer column sits
    # at gx=-5/+5 so the forearm width matches the pauldron block.
    fill_box(-5, 4, 0, 2, 4, 2)        # left forearm + gauntlet
    fill_box( 4, 4, 0, 2, 4, 2)        # right forearm + gauntlet
    # Gauntlet fists — protrude one voxel forward.
    fill_box(-5, 3, -1, 2, 1, 2)
    fill_box( 4, 3, -1, 2, 1, 2)

    # --- Thick exosuit legs (gy=1..2) ---
    # 2 voxels wide each, 2 tall — chunky pistons feeding into big boots.
    fill_box(-2, 1, -1, 2, 2, 2)       # left leg
    fill_box( 1, 1, -1, 2, 2, 2)       # right leg

    # --- Big square industrial boots (gy=0) ---
    # 2 wide x 1 tall x 4 deep — bigger footprint than the legs so the boots
    # flare out as armored stompers. Extends gz=-2 (toe forward of the body)
    # for that planted, top-heavy look.
    fill_box(-2, 0, -2, 2, 1, 4)
    fill_box( 1, 0, -2, 2, 1, 4)

    # uv_overrides: power-pack voxels share (gx, gy) columns with the torso,
    # so without remapping they'd paint the suit-orange torso color. Park
    # them on a dedicated pack pixel in the skin (gunmetal + cyan vent),
    # following the same pattern as the warrior cape.
    # gx range is [-6, 6] (w=13), gy range is [0, 16] (h=17). We reserve the
    # column gx=6 (px=12) above the body for pack pixels:
    #   pack body  -> (6, 14)  gunmetal frame
    #   pack clamp -> (6, 13)  darker clamp
    #   pack vent  -> (6, 15)  single cyan glow column on the back
    uv_overrides = {}
    for v in pack_voxels:
        if v in pack_clamps:
            uv_overrides[v] = (6, 13)
        elif v[0] == 0 and v[2] == 2:
            # Center column of the pack body = cyan glow vent.
            uv_overrides[v] = (6, 15)
        else:
            uv_overrides[v] = (6, 14)

    # Grid: gx in [-6, 6] (w=13), gy in [0, 16] (h=17)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs),
                    uv_overrides=uv_overrides)
    return mb


def gen_player_marksman(height=1.8):
    """Player Marksman class — old-west sniper / frontier gunslinger.

    Tall, lean silhouette built around three readable shapes:
      * A wide-brimmed hat that overhangs the head by one voxel each side
        (the defining feature — must read clearly as a hat from any angle).
      * A long tan duster coat that flares one voxel wider at the hem so
        the lower silhouette tapers outward toward mid-calf.
      * A chest bandolier of brass cartridges across the torso.
    Plus a monocular scope-goggle protruding from the right eye and narrow
    heeled boots peeking out from under the coat hem.
    Origin at feet (Y=0).
    """
    mb = MeshBuilder()
    vs = height / 16.0  # 16-voxel base height; hat adds rows above gy=14
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Heeled boots (gy=0..1) ---
    # Narrow 1-voxel-wide boots at gx=-1/+1. Toe extends forward (gz=-1) and
    # a small heel block sits at gz=1 — gives the "cowboy heel" outline.
    fill_box(-1, 0, -1, 1, 1, 3)         # left boot (toe to heel, gy=0)
    fill_box( 1, 0, -1, 1, 1, 3)         # right boot
    filled.add((-1, 1, 1))               # left heel riser (back-only)
    filled.add(( 1, 1, 1))               # right heel riser

    # --- Coat hem flare (gy=2..3) ---
    # 5 voxels wide (gx=-2..2) — one voxel wider per side than the body
    # column above. This is the "duster flares slightly at the bottom"
    # silhouette and sits at mid-calf height.
    fill_box(-2, 2, -1, 5, 2, 3)

    # --- Lower coat / thigh column (gy=4..5) ---
    # Body-width (3 wide gx=-1..1) so the flare reads as a separate flare
    # rather than just a continuous trapezoid.
    fill_box(-1, 4, -1, 3, 2, 3)

    # --- Torso (gy=6..10) ---
    # Lean 3-wide chest. Bandolier strap row at gy=8 is the same width —
    # the skin texture paints that row as leather + brass cartridges.
    fill_box(-1, 6, -1, 3, 5, 3)

    # --- Shoulder caps (gy=10) ---
    # One-voxel outer shoulder caps at gx=-2 / +2 so the arms have something
    # to attach under and the silhouette doesn't look pin-headed.
    filled.add((-2, 10, 0)); filled.add((-2, 10, 1))
    filled.add(( 2, 10, 0)); filled.add(( 2, 10, 1))

    # --- Arms (gy=4..9) ---
    # Slim 1-voxel-thick arms hanging at gx=-2 / +2 — long & lean.
    fill_box(-2, 7, 0, 1, 3, 1)          # left upper arm
    fill_box( 2, 7, 0, 1, 3, 1)          # right upper arm
    fill_box(-2, 4, 0, 1, 3, 1)          # left forearm
    fill_box( 2, 4, 0, 1, 3, 1)          # right forearm
    # Hands protrude forward (gz=-1) — looks like fists/glove cuffs.
    fill_box(-2, 3, -1, 1, 1, 2)
    fill_box( 2, 3, -1, 1, 1, 2)

    # --- Neck (gy=11) ---
    fill_box(0, 11, 0, 1, 1, 2)

    # --- Head (gy=12..14) ---
    # 3-wide x 3-deep skull (gx=-1..1, gz=-1..1). Narrower than the brim so
    # the brim visibly overhangs on every side.
    fill_box(-1, 12, -1, 3, 3, 3)
    # Eye sockets — front-face carve only, like the other humanoid heads.
    filled.discard((-1, 13, -1))
    filled.discard(( 1, 13, -1))
    # Mouth — small carve at chin row centre.
    filled.discard(( 0, 12, -1))

    # Hat shadow notch: carve the front-face voxels of the TOP head row so
    # there's a deep recess directly under the brim. The brim sits at gy=15
    # and the eye-side of gy=14 reads as a shadowed band beneath it.
    for gx in range(-1, 2):
        filled.discard((gx, 14, -1))

    # --- Scope goggle (single voxel protrusion at gy=13, right side) ---
    # Sticks out one voxel in front of the head's front face (gz=-2). UV is
    # overridden below so this voxel reads as gunmetal grey rather than
    # picking up the skin colour of the right-eye column.
    SCOPE = (1, 13, -2)
    filled.add(SCOPE)

    # --- Wide-brimmed hat (gy=15) ---
    # 5x1x5 flat disc — overhangs the 3x3 head by ONE voxel in every
    # horizontal direction. This is the defining silhouette feature; with
    # the brim 2 voxels wider than the head from any side view, the figure
    # reads unambiguously as a hat-wearer.
    fill_box(-2, 15, -2, 5, 1, 5)

    # --- Hat crown (gy=16..17) ---
    # 3-wide x 2-tall x 3-deep block centred above the brim.
    fill_box(-1, 16, -1, 3, 2, 3)

    # --- UV overrides ---
    # The scope voxel shares (gx, gy) = (1, 13) with the head's right-eye
    # column, so without an override it would sample the same pixel as the
    # face skin. We point it at (2, 17): that grid cell is unoccupied
    # (crown only fills gx=-1..1 at gy=17) so we can dedicate it to the
    # gunmetal scope colour in the skin texture.
    uv_overrides = {SCOPE: (2, 17)}
    # Back-of-head voxels in the eye columns should not show the dark eye
    # pixel on their rear faces. gz=-1 was discarded (eye socket), but
    # gz=0/1 (back of head) need a remap to a non-eye pixel.
    for gz in range(0, 2):
        uv_overrides[(-1, 13, gz)] = (0, 13)   # neutral face skin pixel
        uv_overrides[( 1, 13, gz)] = (0, 13)
    # Heel risers at gy=1 share columns with the boot row at gy=0 already,
    # but py=1 is otherwise unoccupied (gap between boots and coat hem) —
    # we paint py=1 as boot leather in the skin, so no override needed.

    # Grid: gx in [-2, 2] (w=5), gy in [0, 17] (h=18)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs),
                    uv_overrides=uv_overrides)
    return mb


def gen_player_tinkerer(height=1.7):
    """Player Tinkerer class — drone-summoning artificer.

    Compact, slightly stocky inventor in a slate-blue mechanic's vest over a
    teal undershirt, leather utility belt with tool pouches, and BRASS GOGGLES
    PUSHED UP ONTO THE FOREHEAD (not over the eyes — that's the Combat
    Engineer's welder goggles). A tiny chrome drone is perched on the right
    shoulder for flavor.

    vs uses height / 15 (chunkier voxels) so the figure reads as shorter and
    blockier than the 16-voxel humanoid baseline. Origin at feet (Y=0).
    """
    mb = MeshBuilder()
    # Chunkier voxels: 15 stacks tall instead of the usual 16 → shorter, stocky
    # silhouette even at a smaller world height (1.7 m vs 1.8 m baseline).
    vs = height / 15.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Boots (gy=0) — extended toe at gz=-2 reads as a workboot tip ---
    fill_box(-1, 0, -1, 1, 1, 2)        # left boot (gx=-1, gz=-1..0)
    fill_box( 1, 0, -1, 1, 1, 2)        # right boot
    filled.add((-1, 0, -2))             # left toe extension forward
    filled.add(( 1, 0, -2))             # right toe extension forward

    # --- Shins (gy=1..2) ---
    fill_box(-1, 1, -1, 1, 2, 2)        # left shin
    fill_box( 1, 1, -1, 1, 2, 2)        # right shin

    # --- Thighs (gy=3) — 2 separate legs so the gap reads ---
    fill_box(-1, 3, -1, 1, 1, 2)
    fill_box( 1, 3, -1, 1, 1, 2)

    # --- Hips (gy=4) — 3 wide solid block bridging the legs ---
    fill_box(-1, 4, -1, 3, 1, 3)

    # --- Utility belt (gy=5) — 4 voxels wide, intentionally asymmetric.
    #     The brief calls for "4 wide at the belt row only" to read stocky,
    #     so we extend one voxel onto the gx=-2 side (tool-loop side).
    fill_box(-2, 5, -1, 4, 1, 3)

    # --- Tool pouches at the FRONT of the belt (gz=-2) — two 1x1x1 cubes
    #     stuck onto the front face. They share (gx, gy) columns with the
    #     belt strap row, so we uv-override them to a TAN pixel below.
    filled.add((-1, 5, -2))             # left front pouch
    filled.add(( 1, 5, -2))             # right front pouch

    # --- Lower / mid torso (gy=6..8) — 3-wide undershirt+vest body.
    #     The skin paints gx=0 column teal (undershirt) and gx=±1 columns
    #     slate-blue (vest), so the vertical centre strip reads as exposed
    #     undershirt between the vest flaps. ---
    fill_box(-1, 6, -1, 3, 3, 3)

    # --- Upper torso (gy=9..10) — 3-wide chest, same width all the way up ---
    fill_box(-1, 9, -1, 3, 2, 3)

    # --- Vest front overlay (gz=-2) — an extra-thin panel on the FRONT face
    #     of the torso suggesting an unbuttoned vest open over the undershirt.
    #     Leaves a 1-voxel vertical gap down the centre (gx=0) so the teal
    #     undershirt column shows through. Vest flaps cover gy=6..10 at gx=-1
    #     and gx=+1 only — those columns are vest-blue in the skin so the
    #     panel reads as a flapping unbuttoned vest layer. ---
    for gy in range(6, 11):
        filled.add((-1, gy, -2))        # left vest flap
        filled.add(( 1, gy, -2))        # right vest flap

    # --- Shoulder caps (gy=10) — single voxel bumps at gx=-2/+2 so the
    #     shoulders aren't paper-thin under the vest. ---
    filled.add((-2, 10, 0))
    filled.add(( 2, 10, 0))

    # --- Drone perch (gx=2, gy=11) — a tiny 1x1x1 cube on the RIGHT shoulder
    #     representing a perched chrome drone (optional flavor). Sits in a
    #     cell that no other voxel occupies, so we just paint that single
    #     pixel chrome-blue in the skin texture (no uv_override needed). ---
    filled.add(( 2, 11, 0))

    # --- Neck (gy=11) ---
    filled.add((0, 11, 0))

    # --- Head (gy=12..14) — 3 wide, 3 tall, 3 deep, exposed face ---
    fill_box(-1, 12, -1, 3, 3, 3)

    # Eye row — carve front face at gy=13 (MIDDLE of head) so the eye dots
    # are clearly visible BELOW the goggles. This is the key visual cue that
    # distinguishes the Tinkerer (goggles pushed up onto the forehead) from
    # the Combat Engineer (goggles down over the eyes).
    filled.discard((-1, 13, -1))
    filled.discard(( 1, 13, -1))

    # Mouth — carve front of gy=12 to leave a maniacal-grin dark slit.
    filled.discard((0, 12, -1))

    # --- Brass goggles pushed UP onto the FOREHEAD (gy=14) ---
    # A 3-wide x 1-tall band protruding 1 voxel forward of the forehead at
    # gz=-2. They sit ABOVE the eye row (gy=13) — CRITICAL distinction from
    # the Engineer's welder goggles, which cover the eyes. The entire gy=14
    # row is painted brass in the skin texture, so the goggle strap wraps
    # around the head (front, sides, back).
    filled.add((-1, 14, -2))
    filled.add(( 0, 14, -2))
    filled.add(( 1, 14, -2))

    # --- Arms (gx=-2 / +2) — continuous columns wrapping the torso ---
    # Upper + lower arm form one column gy=6..9. Hand at gy=4 protrudes
    # forward (gz=-1..0) for a fist-like read. gy=5 of the arm column
    # coincides with the belt row → that pixel is dark leather (cuff strap
    # reading), which still works visually.
    fill_box(-2, 6, 0, 1, 4, 1)         # left arm column
    fill_box( 2, 6, 0, 1, 4, 1)         # right arm column
    fill_box(-2, 5, 0, 1, 1, 1)         # left wrist (belt-row column = leather)
    fill_box( 2, 5, 0, 1, 1, 1)         # right wrist
    fill_box(-2, 4, -1, 1, 1, 2)        # left hand (forward toe)
    fill_box( 2, 4, -1, 1, 1, 2)        # right hand

    # --- UV overrides ---
    # Pouches share (gx, gy) columns with the belt strap (gy=5), which the
    # skin paints DARK LEATHER. To make the pouches a distinct TAN highlight
    # we redirect their UV to a non-sampled pixel cell. (gx=-2, gy=11) is
    # free: gx=-2 has no voxel above the shoulder cap at gy=10. We paint
    # that pixel TAN in the skin texture.
    uv_overrides = {}
    uv_overrides[(-1, 5, -2)] = (-2, 11)
    uv_overrides[( 1, 5, -2)] = (-2, 11)

    # Grid: gx in [-2, 2] (w=5), gy in [0, 14] (h=15)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs),
                    uv_overrides=uv_overrides)
    return mb


def gen_player_wanderer(height=1.8):
    """Player Wanderer class — drifter blade-dancer in a tattered robe.

    Lean / athletic silhouette (intentionally NOT a mage):
      - 3-wide torso, 4-wide robe at the hips.
      - Knee-length tattered traveler's robe (ragged hem, NOT floor-length).
      - Bare lower legs visible below the hem (no boots).
      - Bare arms with a cloth wrap band on the forearm-to-elbow stretch.
      - Dust-grey scarf wrapping the lower half of the face (mid-nose to chin).
      - Diagonal shoulder-strap satchel across the chest with a small pouch
        protruding at the right hip.
    Origin at feet (Y=0).
    """
    mb = MeshBuilder()
    # 16-voxel humanoid scale — same as gen_humanoid so weapon mounts and
    # the standard limb attach points line up with the rest of the rig.
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (gy=12..15) ---
    # 5 wide x 4 tall x 4 deep — matches the standard humanoid head box.
    fill_box(-2, 12, -2, 5, 4, 4)
    # Eye sockets — carve the FRONT face only of the eye row (gy=14) so the
    # eye pixel doesn't bleed onto side/back voxels.
    filled.discard((-1, 14, -2))
    filled.discard(( 1, 14, -2))
    # The scarf wrapping the lower half of the face (gy=12..13) is painted
    # in by the skin texture — no extra geometry layer needed because every
    # voxel in a (gx, gy) column samples the same pixel.

    # --- Neck (gy=11) — slim, sits just under the scarf hem ---
    fill_box(0, 11, -1, 1, 1, 2)

    # --- Slim 3-wide torso (gy=8..10) ---
    # Lean dancer build: gx=-1..1, 3 voxels deep.
    fill_box(-1, 8, -1, 3, 3, 3)

    # --- Hip / lower-robe widening (gy=4..7) ---
    # Robe widens to 4 voxels at the hips: gx=-2..1. Reads as draped fabric
    # over a slim waist rather than a tight tunic.
    fill_box(-2, 4, -1, 4, 4, 3)

    # --- Ragged knee-length robe hem (gy=2..3) ---
    # The hem is NOT a clean horizontal cut — a deterministic tatter pattern
    # leaves some columns dangling to gy=2 and others terminating at gy=3.
    # All hem voxels are 3 deep so the silhouette stays consistent from any
    # view angle. NOTE: hem skips gx=-2 / gx=2 — those columns already hold
    # the hand fist at gy=3, so reusing those (gx, gy) cells would force one
    # pixel to colour both hem fabric and skin. Keeping hem inside gx=-1..1
    # avoids that collision.
    hem_full_row     = [-1, 0, 1]    # inner robe cols reach gy=3
    hem_long_tatters = [-1, 1]       # tattered cols dangle further to gy=2
    for gx in hem_full_row:
        for gz in range(-1, 2):
            filled.add((gx, 3, gz))
    for gx in hem_long_tatters:
        for gz in range(-1, 2):
            filled.add((gx, 2, gz))

    # --- Bare lower legs (gy=0..1) ---
    # Two slim 1-voxel shins. Visible below the robe hem, NO boots — keeps
    # the wandering-martial-artist read instead of a soldier silhouette.
    fill_box(-1, 0, 0, 1, 2, 1)
    fill_box( 1, 0, 0, 1, 2, 1)

    # --- Slim bare arms ---
    # Arms hang from the shoulders (gy=10) down to fists at gy=3, at gx=-2 /
    # gx=2 (one column outside the slim torso). 1 voxel wide & deep — keeps
    # the dancer silhouette. The forearm-to-elbow wrap band (gy=4..6) is a
    # purely skin-painted stripe over the bare-arm columns — no geometry
    # change because every voxel in a column samples the same pixel.
    fill_box(-2, 8, 0, 1, 3, 1)   # left upper arm  (gy=8..10) — bare skin
    fill_box( 2, 8, 0, 1, 3, 1)   # right upper arm
    fill_box(-2, 4, 0, 1, 4, 1)   # left lower arm  (gy=4..7) — forearm
    fill_box( 2, 4, 0, 1, 4, 1)   # right lower arm
    # Hands (small fists protruding forward).
    fill_box(-2, 3, -1, 1, 1, 2)
    fill_box( 2, 3, -1, 1, 1, 2)

    # --- Satchel pouch — one protruding voxel at the right hip ---
    # Sits in front of the robe at gx=1, gy=5, gz=-2 (one voxel forward of
    # the robe's front face at gz=-1). Reads as a small pouch hanging off
    # the shoulder strap.
    filled.add((1, 5, -2))

    # --- UV overrides ---
    # The skin is one pixel per (gx, gy) — voxels that need to break out of
    # their natural column colour must be remapped. Two cases:
    #   1. Back of head in the eye row (gy=14): without a remap the eye
    #      pixel bleeds to side and back faces. Re-aim those voxels at the
    #      hair colour cell (gx=0, gy=15) — the top of the head row.
    #   2. The satchel strap is a diagonal stripe of TORSO voxels recoloured
    #      to leather brown. We aim those at a dedicated "strap" pixel at
    #      (gx=-2, gy=0): gx=-2 only carries the head row (gy=12..15) and
    #      hip/robe rows (gy=2..10), so gy=0 in that column is empty and
    #      that pixel cell is unsampled by any other voxel — making it a
    #      safe target for the leather-strap colour.
    uv_overrides = {}
    for gz in range(0, 2):
        uv_overrides[(-1, 14, gz)] = (0, 15)
        uv_overrides[( 1, 14, gz)] = (0, 15)

    strap_pixel = (-2, 0)   # unsampled cell — see note above
    strap_voxels = [
        (-1, 10, -1),       # over the left shoulder front
        (-1,  9, -1),
        ( 0,  8, -1),
        ( 0,  7, -1),
        ( 1,  6, -1),
        ( 1,  5, -1),       # right hip front
        ( 1,  5, -2),       # satchel pouch (the protruded voxel)
    ]
    for v in strap_voxels:
        uv_overrides[v] = strap_pixel

    # Grid: gx in [-2, 2] (w=5), gy in [0, 15] (h=16)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs),
                    uv_overrides=uv_overrides)
    return mb


def gen_player_sorcerer(height=1.8):
    """Player Sorcerer class - robed arcane scholar with a pointed deep hood.

    Tall, lean silhouette:
      * Continuous robe skirt (no separate legs) that flares 1 voxel wider at
        the hem so the bottom reads as a fabric bell.
      * Wide sash band at the waist (separate brighter palette row).
      * Full-length sleeves that meet the robe - no exposed forearms / hands.
      * Tall pointed hood that tapers from 5 wide to a single voxel tip 3
        rows above the head; the carved front of the hood shows deep shadow
        with only two glowing eye voxels visible.
    Origin at feet (Y=0). Reads slightly taller and more imposing than the
    16-voxel humanoid because the hood tip extends well above the head.
    """
    mb = MeshBuilder()
    # 18 voxels tall - the hood's pointed tip needs 2-3 rows above the head,
    # so we use a smaller per-voxel world size than the 16-voxel humanoid in
    # order to keep the body proportions natural while giving the hood
    # headroom above the skull.
    vs = height / 18.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Robe hem flare (gy=0) ---
    # One voxel wider on each side than the main skirt so the robe reads as
    # flared at the bottom (one continuous mass - no separate legs).
    fill_box(-3, 0, -1, 7, 1, 3)

    # --- Robe skirt (gy=1..6) ---
    # 5-wide continuous column down the body. No legs are carved out - the
    # robe is solid all the way to the floor.
    fill_box(-2, 1, -1, 5, 6, 3)

    # --- Sash band (gy=7) ---
    # One row across the waist. Its (gx,gy) pixels get a brighter blue-purple
    # palette in the skin so the sash reads as a distinct accent stripe.
    fill_box(-2, 7, -1, 5, 1, 3)

    # --- Torso / chest (gy=8..11) ---
    # 5-wide above the sash - same width as the skirt so the robe reads as
    # a single continuous garment rather than a tunic-over-skirt.
    fill_box(-2, 8, -1, 5, 4, 3)

    # --- Sleeves (gy=4..11) ---
    # Single-column sleeves on each side, hanging from the shoulder down to
    # where they meet the robe skirt at gy=4. No exposed forearms or hands
    # (the cuff disappears into the robe). Placed at gx=-3 / gx=3, one
    # column beyond the torso width.
    fill_box(-3, 4, 0, 1, 8, 1)
    fill_box( 3, 4, 0, 1, 8, 1)

    # --- Hood shell (gy=12..14) ---
    # 5 wide, 3 deep - thick fabric on all sides of the (unseen) head. The
    # front face of the lower part is carved below to form the face opening.
    fill_box(-2, 12, -1, 5, 3, 3)

    # --- Hood narrowing rows (gy=15..17) ---
    # Each row drops voxels of width/depth to taper the hood to a point
    # 3 voxels above the top of the head - that pointed-wizard-hood
    # silhouette is what makes the figure read taller than baseline.
    fill_box(-1, 15, 0, 3, 1, 2)   # gy=15 - 3 wide, 2 deep
    fill_box( 0, 16, 0, 1, 1, 1)   # gy=16 - single voxel
    fill_box( 0, 17, 0, 1, 1, 1)   # gy=17 - pointed tip

    # --- Carve the face opening (front face of hood, gy=12..13) ---
    # Discarding only the gz=-1 voxels exposes the gz=0 voxels behind them,
    # which we paint as deep shadow in the skin - giving the illusion of a
    # recessed face inside the hood. We KEEP gz=-1 at the two eye columns
    # (gx=-1 and gx=1 at gy=13) so a single bright voxel reads as a glowing
    # eye per side. Side faces of the eye voxel leak cyan, but that reads
    # as spell-light rather than a bug.
    for gx in range(-2, 3):
        filled.discard((gx, 12, -1))      # whole lower face row -> pure shadow
    for gx in [-2, 0, 2]:
        filled.discard((gx, 13, -1))      # face row at eye height, sans eyes
    # (gx=-1, gy=13, gz=-1) and (gx=1, gy=13, gz=-1) remain - eye voxels.

    # --- UV overrides --------------------------------------------------------
    # The shadow palette at (gx, gy=12..13) is meant only for the INSIDE of
    # the hood (the gz=0 voxels visible through the carved front). The back
    # voxels (gz=1) at those columns would otherwise show shadow on the rear
    # of the hood - remap them to a normal robe-purple pixel by sampling
    # (gx, gy=10), which lies in the plain robe-fabric row.
    uv_fix = {}
    for gx in range(-2, 3):
        uv_fix[(gx, 12, 1)] = (gx, 10)    # back of hood at gy=12 -> robe
        uv_fix[(gx, 13, 1)] = (gx, 10)    # back of hood at gy=13 -> robe
    # The gz=0 voxel directly behind each cyan eye voxel shares the eye's
    # (gx, gy=13) column, which would paint it cyan. Remap to the shadow
    # column (gx, gy=12) so the inside of the hood stays dark behind eyes.
    uv_fix[(-1, 13, 0)] = (-1, 12)
    uv_fix[( 1, 13, 0)] = ( 1, 12)
    # And the back-of-hood voxel behind each eye should be robe, not cyan.
    uv_fix[(-1, 13, 1)] = (-1, 10)
    uv_fix[( 1, 13, 1)] = ( 1, 10)

    # Grid: gx in [-3, 3] (w=7), gy in [0, 17] (h=18)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs),
                    uv_overrides=uv_fix)
    return mb


def gen_player_ranger(height=1.8):
    """Player Ranger class — wiry forest scout in a hooded leaf-trimmed cloak.

    Silhouette goals (vs. baseline humanoid):
      * Narrow 3-wide torso so the figure reads as lean / agile.
      * Hood that covers the top, sides, and back of the head but leaves
        the FRONT face open so the player still sees a face. Mossy-green
        cloak, leather boots/gloves, leaf-fringed cape down the back.
      * Two short quiver rods bumping up off the upper back behind the
        RIGHT shoulder (player-right == +gx). Reads as "arrows on back".
      * Slight slouch — the chest sits one voxel forward of where the
        warrior's chest would (gz=-2..0 instead of gz=-1..1), suggesting
        a hunched / stalking posture.
    Origin at feet (Y=0).
    """
    mb = MeshBuilder()
    vs = height / 16.0  # 16-voxel-tall baseline, same as archer/rogue
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (5 wide, like archer/rogue so the face row centers correctly) ---
    # gy=13..15 main skull, gy=12 chin. gz=-2 is the FRONT face.
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)
    # Eye sockets — front face only, so the eye pixel stays on the front
    # and doesn't bleed to the side/back of the head.
    filled.discard((-1, 14, -2))
    filled.discard(( 1, 14, -2))
    # Mouth — front-face carve on the chin row.
    filled.discard((0, 12, -2))

    # --- Hood ---
    # Hood TOP sits directly above the head (gy=16..17). The skin texture
    # paints those rows mossy-green so the whole block reads as fabric.
    fill_box(-2, 16, -2, 5, 2, 4)
    # Hood SIDE cheek drapes — single-voxel columns of hood fabric at
    # gx=-2 and gx=+2 BELOW the head (gy=11..12) so the hood frames past
    # the chin. The outer columns of the head itself (gx=±2 at gy=13..15)
    # read as hood directly via the skin texture, so no extra voxels are
    # needed up there.
    fill_box(-2, 11, -2, 1, 2, 4)
    fill_box( 2, 11, -2, 1, 2, 4)
    # Hood BACK — wraps the rear of the head at gz=2 so the back is
    # enclosed; the front (gz=-2) stays open so the face shows.
    fill_box(-2, 12, 2, 5, 4, 1)

    # --- Neck ---
    fill_box(0, 11, 0, 1, 1, 1)

    # --- Slim torso (3 wide vs. archer's 4 / paladin's 7) ---
    # Slouch encoded as shifted Z: torso occupies gz=-2..0 instead of the
    # warrior's gz=-1..1 — the chest sticks a voxel forward.
    fill_box(-1, 8, -2, 3, 3, 3)
    # Tiny shoulder caps so arms don't look stuck onto a stick.
    fill_box(-2, 9, -1, 1, 1, 2)
    fill_box( 2, 9, -1, 1, 1, 2)

    # --- Waist + belt (still 3 wide) ---
    fill_box(-1, 6, -2, 3, 2, 3)
    fill_box(-1, 5, -2, 3, 1, 3)  # belt row — painted leather brown

    # --- Hips (3 wide — wiry build doesn't widen at the hips) ---
    fill_box(-1, 4, -2, 3, 1, 3)

    # --- Cape (mid-spine to lower back, leaf-fringed bottom) ---
    # Cape sits one voxel BEHIND the torso (gz=2 — torso back face is at
    # gz=0, gz=1 would be flush with the back, gz=2 reads as a distinct
    # cape layer). Spans gy=5..10 across gx=-1..1 (matches torso width).
    cape_voxels = []
    for gy in range(5, 11):
        for gx in range(-1, 2):
            v = (gx, gy, 2)
            filled.add(v); cape_voxels.append(v)
    # Fringe — vary the bottom of the cape by ±1 voxel per column so the
    # edge looks ragged / leaf-trimmed rather than a clean horizontal line.
    filled.discard((-1, 5, 2))                    # tuck the left column up one row
    extra = [(-1, 4, 2), (0, 4, 2), (0, 3, 2)]    # dangling fringe drops
    for v in extra:
        filled.add(v); cape_voxels.append(v)

    # Leaf-accent voxels — recolor the lowest center fringe drops to
    # yellow-green so the cape edge gets a flash of autumnal color.
    leaf_voxels = [(0, 4, 2), (0, 3, 2)]

    # --- Quiver rods (behind RIGHT shoulder, sticking up from upper back) ---
    # Right shoulder is at gx=+1..+2. Two parallel rods at gz=2 (behind the
    # back) at gy=11..13. Different heights so they read as two arrows of
    # slightly different lengths poking up out of a quiver.
    quiver_voxels = []
    for v in [(1, 11, 2), (1, 12, 2), (1, 13, 2),
              (2, 11, 2), (2, 12, 2)]:
        filled.add(v); quiver_voxels.append(v)

    # --- Arms (slim, 1-voxel thick, outer face at gx=-3 / gx=+3) ---
    fill_box(-3, 7, -1, 1, 3, 2)   # left upper arm
    fill_box( 3, 7, -1, 1, 3, 2)   # right upper arm
    fill_box(-3, 4, -1, 1, 3, 2)   # left lower arm
    fill_box( 3, 4, -1, 1, 3, 2)   # right lower arm
    # Gloves — protrude forward (gz=-2) for clearer hand silhouette.
    fill_box(-3, 3, -2, 1, 1, 2)
    fill_box( 3, 3, -2, 1, 1, 2)

    # --- Legs (slim) ---
    fill_box(-2, 2, -1, 2, 2, 2)   # left thigh
    fill_box( 1, 2, -1, 2, 2, 2)   # right thigh
    fill_box(-2, 0, 0, 1, 2, 1)    # left shin
    fill_box( 1, 0, 0, 1, 2, 1)    # right shin
    # Light boots — short toe flap forward (gz=-1), no heavy plating.
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box( 1, 0, -1, 1, 1, 2)

    # --- UV overrides ---
    # Cape and quiver voxels at gz=2 share (gx,gy) with belt/waist/torso
    # columns that get other colors (leather belt etc.), so we remap them
    # to dedicated pixels in the top-right corner of the texture — those
    # grid cells aren't touched by any other voxel.
    # Grid is gx in [-3, 3], gy in [0, 17] → pixel (gx+3, gy). Unused
    # top-right cells: (gx=3, gy=15..17) — no body voxels at those (gx,gy).
    uv_fix = {}
    # Eye-column UV bookkeeping:
    #   gz=-2 → discarded (eye-socket carve, no voxel).
    #   gz=-1 → visible eye voxel; keep its native UV so the front face
    #           samples the eye pixel.
    #   gz=0,1 → head interior; their external faces are all internal,
    #           so an override is purely cosmetic safety.
    for gz in (0, 1):
        uv_fix[(-1, 14, gz)] = (0, 14)
        uv_fix[( 1, 14, gz)] = (0, 14)
    # Hood BACK layer (gz=2, gy=12..15, gx=-2..2) — its back face is the
    # only externally-visible face per voxel, and the natural pixel for
    # several of those (gx,gy) cells is skin or eye color. Override the
    # whole layer to the cloak-green pixel so the back of the hood reads
    # uniformly as fabric.
    for hy in range(12, 16):
        for hx in range(-2, 3):
            uv_fix[(hx, hy, 2)] = (3, 17)
    # Cape body → cloak-green pixel at (3, 17).
    for v in cape_voxels:
        uv_fix[v] = (3, 17)
    # Leaf accents override the cape body for two voxels → (3, 16).
    for v in leaf_voxels:
        uv_fix[v] = (3, 16)
    # Quivers → quiver brown pixel at (3, 15).
    for v in quiver_voxels:
        uv_fix[v] = (3, 15)

    # Grid: gx in [-3, 3] (w=7), gy in [0, 17] (h=18)
    add_voxel_model(mb, filled, vs, offset=(-0.5 * vs, 0, -0.5 * vs),
                    uv_overrides=uv_fix)
    return mb


MESH_TYPES = {
    "humanoid": {
        "func": gen_humanoid,
        "desc": "Barony-style voxel humanoid. Params: --height",
        "default_file": "humanoid.obj",
    },
    "humanoid_torso": {
        "func": gen_humanoid_torso,
        "desc": "Skeleton torso only (no arms/legs). Params: --height",
        "default_file": "skeleton.obj",
    },
    "human": {
        "func": gen_human,
        "desc": "Barony-style voxel human NPC. Params: --height",
        "default_file": "human.obj",
    },
    "human_torso": {
        "func": gen_human_torso,
        "desc": "Human NPC torso only (no arms/legs). Params: --height",
        "default_file": "human.obj",
    },
    "butcher": {
        "func": lambda height=2.5: gen_butcher(height),
        "desc": "Large demon boss with horns. Params: --height",
        "default_file": "butcher.obj",
    },
    "butcher_torso": {
        "func": lambda height=2.5: gen_butcher_torso(height),
        "desc": "Butcher torso only (no arms/legs). Params: --height",
        "default_file": "butcher.obj",
    },
    "skeleton_arm": {
        "func": gen_skeleton_arm,
        "desc": "Skeleton arm (upper + lower + hand). Params: --height",
        "default_file": "skeleton_arm.obj",
    },
    "skeleton_leg": {
        "func": gen_skeleton_leg,
        "desc": "Skeleton leg (thigh + shin + foot). Params: --height",
        "default_file": "skeleton_leg.obj",
    },
    "bat_wing": {
        "func": lambda wingspan=1.0: gen_bat_wing(wingspan),
        "desc": "Bat wing membrane shape. Params: --wingspan",
        "default_file": "bat_wing.obj",
    },
    "pit_fiend_wing": {
        "func": lambda size=0.4: gen_pit_fiend_wing(size),
        "desc": "Small triangular wing plate for pit fiend. Params: --size",
        "default_file": "pit_fiend_wing.obj",
    },
    "butcher_arm": {
        "func": lambda height=2.5: gen_butcher_arm(height),
        "desc": "Butcher thick demon arm. Params: --height",
        "default_file": "butcher_arm.obj",
    },
    "butcher_leg": {
        "func": lambda height=2.5: gen_butcher_leg(height),
        "desc": "Butcher thick demon leg. Params: --height",
        "default_file": "butcher_leg.obj",
    },
    "bat_foot": {
        "func": gen_bat_foot,
        "desc": "Bat foot/talon with three toes.",
        "default_file": "bat_foot.obj",
    },
    "spider_leg_pair": {
        "func": lambda radius=0.6: gen_spider_leg_pair(radius),
        "desc": "Spider leg pair (2 legs, left side). Params: --radius",
        "default_file": "spider_leg_pair.obj",
    },
    "andariel": {
        "func": lambda height=2.0: gen_andariel(height),
        "desc": "Andariel spider-woman boss. Params: --height",
        "default_file": "andariel.obj",
    },
    "cleric": {
        "func": gen_cleric,
        "desc": "Male cleric NPC with hood and robes. Params: --height",
        "default_file": "cleric.obj",
    },
    "archer": {
        "func": gen_archer,
        "desc": "Female archer NPC with ponytail and quiver. Params: --height",
        "default_file": "archer.obj",
    },
    "mage": {
        "func": gen_mage,
        "desc": "Male mage NPC with wizard hat and robes. Params: --height",
        "default_file": "mage.obj",
    },
    "rogue": {
        "func": gen_rogue,
        "desc": "Male rogue NPC with hood and cape. Params: --height",
        "default_file": "rogue.obj",
    },
    "paladin": {
        "func": gen_paladin,
        "desc": "Male paladin NPC with heavy plate armor and helm. Params: --height",
        "default_file": "paladin.obj",
    },
    "player_warrior": {
        "func": gen_player_warrior,
        "desc": "Player Warrior: plate armor + cape + horn-slit helm. Params: --height",
        "default_file": "player_warrior.obj",
    },
    "player_paladin": {
        "func": gen_player_paladin,
        "desc": "Player Paladin: white-gold plate + winged helm + tabard. Params: --height",
        "default_file": "player_paladin.obj",
    },
    "player_rogue": {"func": gen_player_rogue, "desc": "Player Rogue: hood + face wrap + knife straps. Params: --height", "default_file": "player_rogue.obj"},
    "player_combat_engineer": {"func": gen_player_combat_engineer, "desc": "Player Combat Engineer: hazard-orange exosuit + power-pack + welder helm. Params: --height", "default_file": "player_combat_engineer.obj"},
    "player_marksman": {"func": gen_player_marksman, "desc": "Player Marksman: duster + wide-brim hat + bandolier + scope-goggle. Params: --height", "default_file": "player_marksman.obj"},
    "player_tinkerer": {"func": gen_player_tinkerer, "desc": "Player Tinkerer: vest + utility belt + brass goggles on forehead. Params: --height", "default_file": "player_tinkerer.obj"},
    "player_wanderer": {"func": gen_player_wanderer, "desc": "Player Wanderer: tattered robe + face scarf + arm wraps. Params: --height", "default_file": "player_wanderer.obj"},
    "player_sorcerer": {"func": gen_player_sorcerer, "desc": "Player Sorcerer: long robe + pointed hood + glowing eyes. Params: --height", "default_file": "player_sorcerer.obj"},
    "player_ranger": {"func": gen_player_ranger, "desc": "Player Ranger: hooded cloak + leaf cape + quiver. Params: --height", "default_file": "player_ranger.obj"},
    "staff": {
        "func": gen_staff,
        "desc": "Staff weapon — thin rod with crystal tip. Params: --height",
        "default_file": "staff.obj",
    },
    "web": {
        "func": gen_web,
        "desc": "Flat spider web decoration (legacy). Params: --radius (used as size)",
        "default_file": "web.obj",
    },
    "web_wall": {
        "func": gen_web_wall,
        "desc": "Wall-mounted spider web — upright panel. Params: --radius (used as size)",
        "default_file": "web_wall.obj",
    },
    "web_ceiling": {
        "func": gen_web_ceiling,
        "desc": "Ceiling spider web — flat quad, visible from below. Params: --radius (used as size)",
        "default_file": "web_ceiling.obj",
    },
    "shackles": {
        "func": gen_shackles,
        "desc": "Wall shackles with chains and cuffs. Params: --height",
        "default_file": "shackles.obj",
    },
    "barrel": {
        "func": gen_barrel,
        "desc": "Simple barrel with rim bands. Params: --radius --height",
        "default_file": "barrel.obj",
    },
    "cage": {
        "func": gen_cage,
        "desc": "Hanging cage with vertical bars. Params: --width --height",
        "default_file": "cage.obj",
    },
    "bones": {
        "func": gen_bones,
        "desc": "Pile of bones on the ground.",
        "default_file": "bones.obj",
    },
    "brazier": {
        "func": gen_brazier,
        "desc": "Standing brazier/fire bowl. Params: --height",
        "default_file": "brazier.obj",
    },
    "spider": {
        "func": gen_spider,
        "desc": "Barony-style voxel spider. Params: --radius",
        "default_file": "spider.obj",
    },
    "bat": {
        "func": gen_bat,
        "desc": "Barony-style voxel bat. Params: --wingspan",
        "default_file": "bat.obj",
    },
    "pillar": {
        "func": gen_pillar,
        "desc": "N-sided cylinder pillar. Params: --radius --height --sides",
        "default_file": "pillar.obj",
    },
    "chest": {
        "func": gen_chest,
        "desc": "Simple openable chest (~24 tris). Params: --width",
        "default_file": "chest.obj",
    },
    "turret": {
        "func": gen_turret,
        "desc": "Combat engineer sentry turret — tripod + barrel. Params: --height",
        "default_file": "turret.obj",
    },
    "gargoyle": {
        "func": gen_gargoyle,
        "desc": "Gargoyle ambush enemy — hunched stone humanoid. Params: --height",
        "default_file": "gargoyle.obj",
    },
    "necromancer": {
        "func": gen_necromancer,
        "desc": "Necromancer — tall hooded skeleton mage. Params: --height",
        "default_file": "necromancer.obj",
    },
    "shaman": {
        "func": gen_shaman,
        "desc": "Shaman healer — stocky with horned headdress. Params: --height",
        "default_file": "shaman.obj",
    },
    "herald": {
        "func": gen_herald,
        "desc": "Herald aura enemy — tall thin skeleton. Params: --height",
        "default_file": "herald.obj",
    },
    "hellhound": {
        "func": gen_hellhound,
        "desc": "Quadruped canine demon — low, long body, snarling. Params: --height",
        "default_file": "hellhound.obj",
    },
    "wraith": {
        "func": gen_wraith,
        "desc": "Ghostly legless wraith — floating torso + trailing arms. Params: --height",
        "default_file": "wraith.obj",
    },
    "sentinel": {
        "func": gen_sentinel,
        "desc": "Armored shield-bearing sentinel — solid plated torso. Params: --height",
        "default_file": "sentinel.obj",
    },
    "cave_troll": {
        "func": lambda height=2.2: gen_cave_troll(height),
        "desc": "Hunched brutish troll — massive shoulders, short legs. Params: --height",
        "default_file": "cave_troll.obj",
    },
    "pit_fiend": {
        "func": lambda height=2.4: gen_pit_fiend(height),
        "desc": "Winged demon with tail and horns. Params: --height",
        "default_file": "pit_fiend.obj",
    },
    "hellforge_smith": {
        "func": lambda height=2.0: gen_hellforge_smith(height),
        "desc": "Hunched demonic blacksmith with hammer arm. Params: --height",
        "default_file": "hellforge_smith.obj",
    },
    "succubus": {
        "func": gen_succubus,
        "desc": "Lithe winged demon — slender with pauldrons and tail. Params: --height",
        "default_file": "succubus.obj",
    },
    "abyssal_titan": {
        "func": lambda height=2.8: gen_abyssal_titan(height),
        "desc": "Massive void colossus — crystal spikes, enormous build. Params: --height",
        "default_file": "abyssal_titan.obj",
    },
    "entropy_weaver": {
        "func": gen_entropy_weaver,
        "desc": "Spindly hooded void caster — extra long arms, robed. Params: --height",
        "default_file": "entropy_weaver.obj",
    },
    "sword": {
        "func": gen_sword,
        "desc": "Sword weapon — blade + handle. Params: --height (as length)",
        "default_file": "sword.obj",
    },
    "dagger": {
        "func": gen_dagger,
        "desc": "Dagger weapon — short blade + grip. Params: --height (as length)",
        "default_file": "dagger.obj",
    },
    "axe": {
        "func": gen_axe,
        "desc": "Axe weapon — asymmetric blade on shaft. Params: --height (as length)",
        "default_file": "axe.obj",
    },
    "claymore": {
        "func": gen_claymore,
        "desc": "Claymore weapon — large two-handed sword. Params: --height (as length)",
        "default_file": "claymore.obj",
    },
    "pistol": {
        "func": gen_pistol,
        "desc": "Pistol weapon — L-shaped handgun. Params: --height (as length)",
        "default_file": "pistol.obj",
    },
    "smg": {
        "func": gen_smg,
        "desc": "SMG weapon — compact automatic with magazine. Params: --height (as length)",
        "default_file": "smg.obj",
    },
    "carbine": {
        "func": gen_carbine,
        "desc": "Carbine weapon — long-barreled rifle. Params: --height (as length)",
        "default_file": "carbine.obj",
    },
    "revolver": {
        "func": gen_revolver,
        "desc": "Revolver weapon — barrel + cylinder + grip. Params: --height (as length)",
        "default_file": "revolver.obj",
    },
    "bow": {
        "func": gen_bow,
        "desc": "Bow weapon — curved arc with grip. Params: --height",
        "default_file": "bow.obj",
    },
    "crossbow": {
        "func": gen_crossbow,
        "desc": "Crossbow weapon — stock with horizontal prods. Params: --width",
        "default_file": "crossbow.obj",
    },
    "throwing_knife": {
        "func": gen_throwing_knife,
        "desc": "Throwing knife — thin blade + pommel. Params: --height (as length)",
        "default_file": "throwing_knife.obj",
    },
    "molotov": {
        "func": gen_molotov,
        "desc": "Molotov cocktail — bottle + neck. Params: --height",
        "default_file": "molotov.obj",
    },
    "chakram": {
        "func": gen_chakram,
        "desc": "Chakram — flat metal throwing ring. Params: --radius",
        "default_file": "chakram.obj",
    },
    "infinity_chakram": {
        "func": gen_infinity_chakram,
        "desc": "Infinity Chakram — two linked rings forming an ∞. Params: --radius",
        "default_file": "infinity_chakram.obj",
    },
    "wand": {
        "func": gen_wand,
        "desc": "Wand weapon — shaft + crystal tip. Params: --height (as length)",
        "default_file": "wand.obj",
    },
    "helmet": {
        "func": gen_helmet,
        "desc": "Armor helmet — dome with rim band.",
        "default_file": "helmet.obj",
    },
    "helmet_light": {
        "func": lambda: gen_helmet(bulk=0.8),
        "desc": "Light helmet — slimmer cloth-cap dome (bulk=0.8).",
        "default_file": "helmet_light.obj",
    },
    "helmet_heavy": {
        "func": lambda: gen_helmet(bulk=1.25),
        "desc": "Heavy helmet — chunkier plate dome (bulk=1.25).",
        "default_file": "helmet_heavy.obj",
    },
    "armor": {
        "func": gen_armor,
        "desc": "Body armor — torso plate with shoulder pads.",
        "default_file": "armor.obj",
    },
    "chest_light": {
        "func": lambda: gen_armor(bulk=0.8),
        "desc": "Light chest — thin cloth chest piece (bulk=0.8).",
        "default_file": "chest_light.obj",
    },
    "chest_heavy": {
        "func": lambda: gen_armor(bulk=1.25),
        "desc": "Heavy chest — thick plate chest with large pauldrons (bulk=1.25).",
        "default_file": "chest_heavy.obj",
    },
    "boots": {
        "func": gen_boots,
        "desc": "Boots — short box with toe extension.",
        "default_file": "boots.obj",
    },
    "boots_light": {
        "func": lambda: gen_boots(bulk=0.8),
        "desc": "Light boots — slim cloth boot (bulk=0.8).",
        "default_file": "boots_light.obj",
    },
    "boots_heavy": {
        "func": lambda: gen_boots(bulk=1.25),
        "desc": "Heavy boots — chunky plated greave (bulk=1.25).",
        "default_file": "boots_heavy.obj",
    },
    "gloves": {
        "func": gen_gloves,
        "desc": "Gloves — paired gauntlets with thumbs and flared cuffs.",
        "default_file": "gloves.obj",
    },
    "gloves_light": {
        "func": lambda: gen_gloves(bulk=0.8),
        "desc": "Light gloves — slim cloth gloves (bulk=0.8).",
        "default_file": "gloves_light.obj",
    },
    "gloves_heavy": {
        "func": lambda: gen_gloves(bulk=1.25),
        "desc": "Heavy gloves — wide plated gauntlets (bulk=1.25).",
        "default_file": "gloves_heavy.obj",
    },
    "ring": {
        "func": gen_ring,
        "desc": "Finger ring — small cylinder.",
        "default_file": "ring.obj",
    },
    "shield": {
        "func": gen_shield,
        "desc": "Off-hand shield — flat rectangle with center boss.",
        "default_file": "shield.obj",
    },
    "mace": {
        "func": gen_mace,
        "desc": "Mace weapon — shaft with heavy head.",
        "default_file": "mace.obj",
    },
    "cleaver": {
        "func": gen_cleaver,
        "desc": "Cleaver — wide blade on short handle.",
        "default_file": "cleaver.obj",
    },
    "iron_maiden": {
        "func": gen_iron_maiden,
        "desc": "Iron maiden torture device — tall coffin box.",
        "default_file": "iron_maiden.obj",
    },
    "arrow": {
        "func": gen_arrow,
        "desc": "Arrow projectile — thin shaft + arrowhead.",
        "default_file": "arrow.obj",
    },
    "bolt": {
        "func": gen_bolt,
        "desc": "Crossbow bolt — short shaft + flat head.",
        "default_file": "bolt.obj",
    },
    # Boss mesh types
    "lich": {
        "func": lambda height=2.0: gen_lich(height),
        "desc": "Hooded skeletal sorcerer boss with flared robe base. Params: --height",
        "default_file": "lich.obj",
    },
    "warden": {
        "func": lambda height=2.4: gen_warden(height),
        "desc": "Armored skeletal tomb-warden with gravestone pauldrons. Params: --height",
        "default_file": "warden.obj",
    },
    "spider_queen": {
        "func": lambda radius=0.7: gen_spider_queen(radius),
        "desc": "Bloated spider queen boss with egg-sac abdomen. Params: --radius",
        "default_file": "spider_queen.obj",
    },
    "korvath": {
        "func": lambda height=2.4: gen_korvath(height),
        "desc": "Armored siege juggernaut with tower shield. Params: --height",
        "default_file": "korvath.obj",
    },
    "azhar": {
        "func": lambda height=2.2: gen_azhar(height),
        "desc": "Lean demon duelist with swept horns and baked blade. Params: --height",
        "default_file": "azhar.obj",
    },
    "diabro": {
        "func": lambda height=2.6: gen_diabro(height),
        "desc": "Iconic demon terror — hunched bestial posture with ridged spine. Params: --height",
        "default_file": "diabro.obj",
    },
    "nyx": {
        "func": lambda height=2.2: gen_nyx(height),
        "desc": "Void weaver boss — crystal crown, spindly torso, trailing robe tendrils. Params: --height",
        "default_file": "nyx.obj",
    },
    "reaper": {
        "func": lambda height=2.6: gen_reaper(height),
        "desc": "Grim Reaper boss — deep-shadow hood, baked scythe, cloak base. Params: --height",
        "default_file": "reaper.obj",
    },
}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate low-poly 3D meshes as OBJ files.")

    parser.add_argument("--list-types", action="store_true",
                        help="List available mesh types and exit.")
    parser.add_argument("--type", choices=list(MESH_TYPES.keys()),
                        help="Mesh type to generate.")
    parser.add_argument("--out", type=str, default=None,
                        help="Output OBJ file path.")

    # Shared parameters
    parser.add_argument("--height", type=float, default=None,
                        help="Height (humanoid, pillar).")
    parser.add_argument("--radius", type=float, default=None,
                        help="Radius (spider, pillar).")
    parser.add_argument("--wingspan", type=float, default=None,
                        help="Wingspan (bat).")
    parser.add_argument("--width", type=float, default=None,
                        help="Width (chest).")
    parser.add_argument("--sides", type=int, default=None,
                        help="Number of sides (pillar).")

    args = parser.parse_args()

    if args.list_types:
        print("Available mesh types:")
        for name, info in MESH_TYPES.items():
            print(f"  {name:12s} — {info['desc']}")
        return

    if not args.type:
        parser.error("--type is required (or use --list-types)")

    # Determine output path
    # Default: assets/meshes/ relative to the game root (script's parent dir)
    game_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_dir = os.path.join(game_root, "assets", "meshes")

    if args.out:
        out_path = args.out
        # If relative, resolve relative to cwd
        if not os.path.isabs(out_path):
            out_path = os.path.abspath(out_path)
    else:
        out_path = os.path.join(default_dir, MESH_TYPES[args.type]["default_file"])

    # Build kwargs for the generator
    kwargs = {}
    mtype = args.type

    if mtype in ("humanoid", "human", "cleric", "archer", "butcher",
                 "mage", "rogue", "paladin", "staff"):
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "spider":
        if args.radius is not None:
            kwargs["radius"] = args.radius
    elif mtype == "bat":
        if args.wingspan is not None:
            kwargs["wingspan"] = args.wingspan
    elif mtype == "pillar":
        if args.radius is not None:
            kwargs["radius"] = args.radius
        if args.height is not None:
            kwargs["height"] = args.height
        if args.sides is not None:
            kwargs["sides"] = args.sides
    elif mtype == "chest":
        if args.width is not None:
            kwargs["width"] = args.width
    elif mtype in ("shackles", "brazier", "turret", "gargoyle", "necromancer", "shaman", "herald"):
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "web":
        if args.radius is not None:
            kwargs["size"] = args.radius
    elif mtype == "barrel":
        if args.radius is not None:
            kwargs["radius"] = args.radius
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "cage":
        if args.width is not None:
            kwargs["width"] = args.width
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype in ("sword", "dagger", "axe", "claymore", "pistol",
                   "smg", "carbine", "revolver", "throwing_knife", "wand"):
        if args.height is not None:
            kwargs["length"] = args.height
    elif mtype in ("bow", "molotov"):
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "crossbow":
        if args.width is not None:
            kwargs["width"] = args.width
    elif mtype == "chakram":
        if args.radius is not None:
            kwargs["radius"] = args.radius
    # bones and brazier use defaults — no special args needed

    mb = MESH_TYPES[mtype]["func"](**kwargs)
    write_obj(out_path, mb)

    tri_count = len(mb.faces)
    vert_count = len(mb.verts)
    print(f"Wrote {out_path}  ({vert_count} verts, {tri_count} tris)")


if __name__ == "__main__":
    main()
