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
        # Boss meshes (floor-milestone encounters)
        ["--type", "lich",        "--height", "2.0", "--out", os.path.join(mesh_dir, "lich.obj")],
        ["--type", "warden",      "--height", "2.4", "--out", os.path.join(mesh_dir, "warden.obj")],
        ["--type", "spider_queen","--radius",  "0.7", "--out", os.path.join(mesh_dir, "spider_queen.obj")],
        ["--type", "korvath",     "--height", "2.4", "--out", os.path.join(mesh_dir, "korvath.obj")],
        ["--type", "azhar",       "--height", "2.2", "--out", os.path.join(mesh_dir, "azhar.obj")],
        ["--type", "diabro",      "--height", "2.6", "--out", os.path.join(mesh_dir, "diabro.obj")],
        ["--type", "nyx",         "--height", "2.2", "--out", os.path.join(mesh_dir, "nyx.obj")],
        ["--type", "reaper",      "--height", "2.6", "--out", os.path.join(mesh_dir, "reaper.obj")],
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
        # Player class meshes (one per PlayerClass enum; renderer resolves the name via
        # ClassDef.meshName in item.h). Distinct from the NPC meshes above on purpose so
        # NPCs and player characters can diverge visually.
        ["--type", "player_warrior",         "--out", os.path.join(mesh_dir, "player_warrior.obj")],
        ["--type", "player_ranger",          "--out", os.path.join(mesh_dir, "player_ranger.obj")],
        ["--type", "player_sorcerer",        "--out", os.path.join(mesh_dir, "player_sorcerer.obj")],
        ["--type", "player_rogue",           "--out", os.path.join(mesh_dir, "player_rogue.obj")],
        ["--type", "player_paladin",         "--out", os.path.join(mesh_dir, "player_paladin.obj")],
        ["--type", "player_combat_engineer", "--out", os.path.join(mesh_dir, "player_combat_engineer.obj")],
        ["--type", "player_marksman",        "--out", os.path.join(mesh_dir, "player_marksman.obj")],
        ["--type", "player_tinkerer",        "--out", os.path.join(mesh_dir, "player_tinkerer.obj")],
        ["--type", "player_wanderer",        "--out", os.path.join(mesh_dir, "player_wanderer.obj")],
        # Archetype enemies
        ["--type", "gargoyle",       "--out", os.path.join(mesh_dir, "gargoyle.obj")],
        ["--type", "necromancer",    "--out", os.path.join(mesh_dir, "necromancer.obj")],
        ["--type", "shaman",         "--out", os.path.join(mesh_dir, "shaman.obj")],
        ["--type", "herald",         "--out", os.path.join(mesh_dir, "herald.obj")],
        # New enemy meshes (roster rework)
        ["--type", "hellhound",      "--height", "2.0", "--out", os.path.join(mesh_dir, "hellhound.obj")],
        ["--type", "wraith",         "--out", os.path.join(mesh_dir, "wraith.obj")],
        ["--type", "sentinel",       "--height", "2.0", "--out", os.path.join(mesh_dir, "sentinel.obj")],
        ["--type", "cave_troll",     "--height", "2.2", "--out", os.path.join(mesh_dir, "cave_troll.obj")],
        ["--type", "pit_fiend",      "--height", "2.4", "--out", os.path.join(mesh_dir, "pit_fiend.obj")],
        ["--type", "pit_fiend_wing", "--out", os.path.join(mesh_dir, "pit_fiend_wing.obj")],
        ["--type", "hellforge_smith", "--height", "2.0", "--out", os.path.join(mesh_dir, "hellforge_smith.obj")],
        ["--type", "succubus",       "--out", os.path.join(mesh_dir, "succubus.obj")],
        ["--type", "abyssal_titan",  "--height", "2.8", "--out", os.path.join(mesh_dir, "abyssal_titan.obj")],
        ["--type", "entropy_weaver", "--out", os.path.join(mesh_dir, "entropy_weaver.obj")],
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
        ["--type", "chakram",        "--out", os.path.join(mesh_dir, "chakram.obj")],
        ["--type", "infinity_chakram", "--out", os.path.join(mesh_dir, "infinity_chakram.obj")],
        # Projectiles
        ["--type", "arrow",          "--out", os.path.join(mesh_dir, "arrow.obj")],
        ["--type", "bolt",           "--out", os.path.join(mesh_dir, "bolt.obj")],
        # Equipment — medium tier (unchanged originals)
        ["--type", "helmet",         "--out", os.path.join(mesh_dir, "helmet.obj")],
        ["--type", "armor",          "--out", os.path.join(mesh_dir, "armor.obj")],
        ["--type", "boots",          "--out", os.path.join(mesh_dir, "boots.obj")],
        ["--type", "gloves",         "--out", os.path.join(mesh_dir, "gloves.obj")],
        # Equipment — light tier (slimmer/cloth, bulk=0.8)
        ["--type", "helmet_light",   "--out", os.path.join(mesh_dir, "helmet_light.obj")],
        ["--type", "helmet_heavy",   "--out", os.path.join(mesh_dir, "helmet_heavy.obj")],
        ["--type", "chest_light",    "--out", os.path.join(mesh_dir, "chest_light.obj")],
        ["--type", "chest_heavy",    "--out", os.path.join(mesh_dir, "chest_heavy.obj")],
        ["--type", "boots_light",    "--out", os.path.join(mesh_dir, "boots_light.obj")],
        ["--type", "boots_heavy",    "--out", os.path.join(mesh_dir, "boots_heavy.obj")],
        ["--type", "gloves_light",   "--out", os.path.join(mesh_dir, "gloves_light.obj")],
        ["--type", "gloves_heavy",   "--out", os.path.join(mesh_dir, "gloves_heavy.obj")],
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
        ("weapon_cleaver_tex",        "weapon_cleaver_tex_42.png"),
        # Equipment skins (gloves; older armor/boots/helmet skins were generated manually)
        ("gloves_plate_tex",          "gloves_plate_tex_42.png"),
        ("gloves_leather_tex",        "gloves_leather_tex_42.png"),
        # Boss skins
        ("boss_andariel",             "boss_andariel_42.png"),
        ("boss_mephisto",             "boss_mephisto_42.png"),
        ("boss_baal",                 "boss_baal_42.png"),
        ("boss_diabro",               "boss_diabro_42.png"),
        ("boss_reaper",               "boss_reaper_42.png"),
        ("boss_lich",                 "boss_lich_42.png"),
        ("boss_spider_queen",         "boss_spider_queen_42.png"),
        ("boss_demon_knight",         "boss_demon_knight_42.png"),
        ("boss_arch_mage",            "boss_arch_mage_42.png"),
        # Enemy archetype skins
        ("gargoyle",                  "gargoyle_skin_42.png"),
        ("necromancer",               "necromancer_skin_42.png"),
        ("cavern_shaman",             "cavern_shaman_skin_42.png"),
        ("crypt_herald",              "crypt_herald_skin_42.png"),
        ("sniper_imp",                "sniper_imp_skin_42.png"),
        ("infernal_herald",           "infernal_herald_skin_42.png"),
        ("void_necromancer",          "void_necromancer_skin_42.png"),
        ("void_shaman",              "void_shaman_skin_42.png"),
        ("void_herald",              "void_herald_skin_42.png"),
        # New enemy-specific skins (roster rework)
        ("bone_archer",              "bone_archer_skin_42.png"),
        ("catacomb_sentinel",        "catacomb_sentinel_skin_42.png"),
        ("tomb_wraith",              "tomb_wraith_skin_42.png"),
        ("plague_bat",               "plague_bat_skin_42.png"),
        ("web_spinner",              "web_spinner_skin_42.png"),
        ("burrowing_widow",          "burrowing_widow_skin_42.png"),
        ("cave_troll",               "cave_troll_skin_42.png"),
        ("pit_fiend",                "pit_fiend_skin_42.png"),
        ("hellforge_smith",          "hellforge_smith_skin_42.png"),
        ("succubus",                 "succubus_skin_42.png"),
        ("entropy_weaver",           "entropy_weaver_skin_42.png"),
        ("nullifier",                "nullifier_skin_42.png"),
        ("mind_flayer",              "mind_flayer_skin_42.png"),
        ("phase_ripper",             "phase_ripper_skin_42.png"),
        ("abyssal_titan",            "abyssal_titan_skin_42.png"),
        # Player class skins (one per PlayerClass; paired with the meshes above).
        ("player_warrior",           "player_warrior_skin_42.png"),
        ("player_ranger",            "player_ranger_skin_42.png"),
        ("player_sorcerer",          "player_sorcerer_skin_42.png"),
        ("player_rogue",             "player_rogue_skin_42.png"),
        ("player_paladin",           "player_paladin_skin_42.png"),
        ("player_combat_engineer",   "player_combat_engineer_skin_42.png"),
        ("player_marksman",          "player_marksman_skin_42.png"),
        ("player_tinkerer",          "player_tinkerer_skin_42.png"),
        ("player_wanderer",          "player_wanderer_skin_42.png"),
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

    # Always generate skill icons header (fast, no args needed)
    if args.all:
        print("\n=== Generating Skill Icons ===")
        ok = run([sys.executable, os.path.join(SCRIPT_DIR, "gen_skill_icons.py")]) and ok

    # Always generate item icon header (tool-built inventory icons → item_icons_gen.h)
    if args.all:
        print("\n=== Generating Item Icons ===")
        ok = run([sys.executable, os.path.join(SCRIPT_DIR, "gen_item_icons.py")]) and ok

    # Compress music WAVs to OGG if they exist (reduces binary size ~10×)
    if args.all:
        import glob
        audio_dir = os.path.join(ROOT_DIR, "assets", "audio")
        music_wavs = glob.glob(os.path.join(audio_dir, "music_*.wav"))
        if music_wavs:
            print("\n=== Compressing Music to OGG ===")
            for wav in music_wavs:
                ogg = wav.replace(".wav", ".ogg")
                if not os.path.exists(ogg):
                    if run(["ffmpeg", "-y", "-i", wav, "-c:a", "libvorbis", "-q:a", "4", ogg]):
                        os.remove(wav)
                    else:
                        print(f"  Warning: ffmpeg failed for {wav}, keeping WAV")

    if ok:
        print("\n=== All assets built successfully ===")
    else:
        print("\n=== Some assets failed to build ===")
        sys.exit(1)

if __name__ == "__main__":
    main()
