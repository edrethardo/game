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
        # Enemy bodies — full body meshes (limbs overlay for animation)
        ["--type", "humanoid", "--height", "1.8", "--out", os.path.join(mesh_dir, "skeleton.obj")],
        ["--type", "spider",   "--radius", "0.6", "--out", os.path.join(mesh_dir, "spider.obj")],
        ["--type", "bat",      "--wingspan", "1.0", "--out", os.path.join(mesh_dir, "bat.obj")],
        ["--type", "butcher",       "--height", "2.5", "--out", os.path.join(mesh_dir, "butcher.obj")],
        ["--type", "andariel",      "--height", "2.0", "--out", os.path.join(mesh_dir, "andariel.obj")],
        # Limb parts
        ["--type", "skeleton_arm",   "--out", os.path.join(mesh_dir, "skeleton_arm.obj")],
        ["--type", "skeleton_leg",   "--out", os.path.join(mesh_dir, "skeleton_leg.obj")],
        ["--type", "bat_wing",       "--out", os.path.join(mesh_dir, "bat_wing_mesh.obj")],
        ["--type", "bat_foot",       "--out", os.path.join(mesh_dir, "bat_foot.obj")],
        ["--type", "butcher_arm",    "--out", os.path.join(mesh_dir, "butcher_arm.obj")],
        ["--type", "butcher_leg",    "--out", os.path.join(mesh_dir, "butcher_leg.obj")],
        ["--type", "spider_leg_pair","--out", os.path.join(mesh_dir, "spider_leg_pair.obj")],
        # NPC class models
        ["--type", "human",    "--out", os.path.join(mesh_dir, "human.obj")],
        ["--type", "cleric",   "--out", os.path.join(mesh_dir, "cleric.obj")],
        ["--type", "archer",   "--out", os.path.join(mesh_dir, "archer.obj")],
        ["--type", "mage",     "--out", os.path.join(mesh_dir, "mage.obj")],
        ["--type", "rogue",    "--out", os.path.join(mesh_dir, "rogue.obj")],
        ["--type", "paladin",  "--out", os.path.join(mesh_dir, "paladin.obj")],
        # Archetype enemies
        ["--type", "gargoyle",       "--out", os.path.join(mesh_dir, "gargoyle.obj")],
        ["--type", "necromancer",    "--out", os.path.join(mesh_dir, "necromancer.obj")],
        ["--type", "shaman",         "--out", os.path.join(mesh_dir, "shaman.obj")],
        ["--type", "herald",         "--out", os.path.join(mesh_dir, "herald.obj")],
        # Weapons
        ["--type", "sword",          "--out", os.path.join(mesh_dir, "sword.obj")],
        ["--type", "dagger",         "--out", os.path.join(mesh_dir, "dagger.obj")],
        ["--type", "axe",            "--out", os.path.join(mesh_dir, "axe.obj")],
        ["--type", "claymore",       "--out", os.path.join(mesh_dir, "claymore.obj")],
        ["--type", "mace",           "--out", os.path.join(mesh_dir, "mace.obj")],
        ["--type", "cleaver",        "--out", os.path.join(mesh_dir, "cleaver.obj")],
        ["--type", "staff",          "--out", os.path.join(mesh_dir, "staff.obj")],
        ["--type", "wand",           "--out", os.path.join(mesh_dir, "wand.obj")],
        ["--type", "pistol",         "--out", os.path.join(mesh_dir, "pistol.obj")],
        ["--type", "smg",            "--out", os.path.join(mesh_dir, "smg.obj")],
        ["--type", "carbine",        "--out", os.path.join(mesh_dir, "carbine.obj")],
        ["--type", "revolver",       "--out", os.path.join(mesh_dir, "revolver.obj")],
        ["--type", "bow",            "--out", os.path.join(mesh_dir, "bow.obj")],
        ["--type", "crossbow",       "--out", os.path.join(mesh_dir, "crossbow.obj")],
        ["--type", "throwing_knife", "--out", os.path.join(mesh_dir, "throwing_knife.obj")],
        ["--type", "molotov",        "--out", os.path.join(mesh_dir, "molotov.obj")],
        # Projectiles
        ["--type", "arrow",          "--out", os.path.join(mesh_dir, "arrow.obj")],
        ["--type", "bolt",           "--out", os.path.join(mesh_dir, "bolt.obj")],
        # Equipment
        ["--type", "helmet",         "--out", os.path.join(mesh_dir, "helmet.obj")],
        ["--type", "armor",          "--out", os.path.join(mesh_dir, "armor.obj")],
        ["--type", "boots",          "--out", os.path.join(mesh_dir, "boots.obj")],
        ["--type", "ring",           "--out", os.path.join(mesh_dir, "ring.obj")],
        ["--type", "shield",         "--out", os.path.join(mesh_dir, "shield.obj")],
        # Props
        ["--type", "pillar",         "--out", os.path.join(mesh_dir, "pillar.obj")],
        ["--type", "chest",          "--out", os.path.join(mesh_dir, "chest.obj")],
        ["--type", "web",            "--out", os.path.join(mesh_dir, "web.obj")],
        ["--type", "shackles",       "--out", os.path.join(mesh_dir, "shackles.obj")],
        ["--type", "barrel",         "--out", os.path.join(mesh_dir, "barrel.obj")],
        ["--type", "cage",           "--out", os.path.join(mesh_dir, "cage.obj")],
        ["--type", "bones",          "--out", os.path.join(mesh_dir, "bones.obj")],
        ["--type", "brazier",        "--out", os.path.join(mesh_dir, "brazier.obj")],
        ["--type", "iron_maiden",    "--out", os.path.join(mesh_dir, "iron_maiden.obj")],
        # Gadgets
        ["--type", "turret",         "--out", os.path.join(mesh_dir, "turret.obj")],
    ]

    ok = True
    for args in meshes:
        if not run([py, tool] + args):
            ok = False
    return ok

def build_skins():
    print("\n=== Building Skins ===")
    py = sys.executable
    tool = os.path.join(SCRIPT_DIR, "gen_skin.py")
    tex_dir = os.path.join(ROOT_DIR, "assets", "textures")

    skins = [
        ("weapon_sword_tex",          "weapon_sword_tex_42.png"),
        ("weapon_dagger_tex",         "weapon_dagger_tex_42.png"),
        ("weapon_axe_tex",            "weapon_axe_tex_42.png"),
        ("weapon_claymore_tex",       "weapon_claymore_tex_42.png"),
        ("weapon_pistol_tex",         "weapon_pistol_tex_42.png"),
        ("weapon_smg_tex",            "weapon_smg_tex_42.png"),
        ("weapon_carbine_tex",        "weapon_carbine_tex_42.png"),
        ("weapon_revolver_tex",       "weapon_revolver_tex_42.png"),
        ("weapon_bow_tex",            "weapon_bow_tex_42.png"),
        ("weapon_crossbow_tex",       "weapon_crossbow_tex_42.png"),
        ("weapon_throwing_knife_tex", "weapon_throwing_knife_tex_42.png"),
        ("weapon_wand_tex",           "weapon_wand_tex_42.png"),
        # Boss skins
        ("boss_andariel",             "boss_andariel_42.png"),
        ("boss_mephisto",             "boss_mephisto_42.png"),
        ("boss_baal",                 "boss_baal_42.png"),
        ("boss_diablo",               "boss_diablo_42.png"),
        ("boss_reaper",               "boss_reaper_42.png"),
        ("boss_lich",                 "boss_lich_42.png"),
        ("boss_spider_queen",         "boss_spider_queen_42.png"),
        ("boss_demon_knight",         "boss_demon_knight_42.png"),
        ("boss_arch_mage",            "boss_arch_mage_42.png"),
        # Enemy archetype skins
        ("gargoyle",                  "gargoyle_skin_42.png"),
        ("necromancer",               "necromancer_skin_42.png"),
        ("cavern_shaman",             "cavern_shaman_skin_42.png"),
        ("infernal_herald",           "infernal_herald_skin_42.png"),
        ("void_necromancer",          "void_necromancer_skin_42.png"),
        ("void_shaman",              "void_shaman_skin_42.png"),
        ("void_herald",              "void_herald_skin_42.png"),
    ]

    ok = True
    for skin_type, filename in skins:
        if not run([py, tool, "--type", skin_type,
                    "--out", os.path.join(tex_dir, filename)]):
            ok = False
    return ok

def main():
    parser = argparse.ArgumentParser(description="Build game assets")
    parser.add_argument("--all", action="store_true", help="Build everything")
    parser.add_argument("--textures", action="store_true", help="Build textures only")
    parser.add_argument("--meshes", action="store_true", help="Build meshes only")
    parser.add_argument("--skins", action="store_true", help="Build weapon skins only")
    args = parser.parse_args()

    if not (args.all or args.textures or args.meshes or args.skins):
        args.all = True

    ok = True
    if args.all or args.textures:
        ok = build_textures() and ok
    if args.all or args.meshes:
        ok = build_meshes() and ok
    if args.all or args.skins:
        ok = build_skins() and ok

    if ok:
        print("\n=== All assets built successfully ===")
    else:
        print("\n=== Some assets failed to build ===")
        sys.exit(1)

if __name__ == "__main__":
    main()
