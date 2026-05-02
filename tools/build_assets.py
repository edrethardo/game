#!/usr/bin/env python3
"""Master asset build script — generates all game assets."""

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)

def run(cmd):
    print(f"  > {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=ROOT_DIR)
    return result.returncode == 0

def build_textures():
    print("\n=== Building Textures ===")
    py = sys.executable
    tool = os.path.join(SCRIPT_DIR, "gen_texture.py")

    textures = [
        ("stone_wall",    42, 32, "dark_dungeon"),
        ("stone_floor",   42, 32, "cold_stone"),
        ("stone_ceiling", 42, 32, "dark_dungeon"),
        ("brick_wall",    42, 32, "warm_brick"),
        ("stone_wall_moss", 7, 32, "dark_dungeon"),
        ("wood_plank",    42, 32, "warm_brick"),
        ("metal_grate",   42, 16, "cold_stone"),
    ]

    ok = True
    for tex_type, seed, size, palette in textures:
        if not run([py, tool, "--type", tex_type, "--seed", str(seed),
                    "--size", str(size), "--palette", palette]):
            ok = False
    return ok

def build_meshes():
    print("\n=== Building Meshes ===")
    py = sys.executable
    tool = os.path.join(SCRIPT_DIR, "gen_mesh.py")
    mesh_dir = os.path.join(ROOT_DIR, "assets", "meshes")

    meshes = [
        ["--type", "humanoid", "--height", "1.8", "--out", os.path.join(mesh_dir, "skeleton.obj")],
        ["--type", "spider",   "--radius", "0.6", "--out", os.path.join(mesh_dir, "spider.obj")],
        ["--type", "bat",      "--wingspan", "1.0", "--out", os.path.join(mesh_dir, "bat.obj")],
        ["--type", "pillar",   "--out", os.path.join(mesh_dir, "pillar.obj")],
        ["--type", "chest",    "--out", os.path.join(mesh_dir, "chest.obj")],
    ]

    ok = True
    for args in meshes:
        if not run([py, tool] + args):
            ok = False
    return ok

def main():
    parser = argparse.ArgumentParser(description="Build game assets")
    parser.add_argument("--all", action="store_true", help="Build everything")
    parser.add_argument("--textures", action="store_true", help="Build textures only")
    parser.add_argument("--meshes", action="store_true", help="Build meshes only")
    args = parser.parse_args()

    if not (args.all or args.textures or args.meshes):
        args.all = True

    ok = True
    if args.all or args.textures:
        ok = build_textures() and ok
    if args.all or args.meshes:
        ok = build_meshes() and ok

    if ok:
        print("\n=== All assets built successfully ===")
    else:
        print("\n=== Some assets failed to build ===")
        sys.exit(1)

if __name__ == "__main__":
    main()
