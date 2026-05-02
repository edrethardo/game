#include "tools/model_gen.h"
#include <cstdio>
#include <cstring>

// Standalone CLI for generating weapon OBJ meshes and icon bitmaps.
//
// Usage:
//   weapon_gen                              # generate missing models
//   weapon_gen --force                      # regenerate all models
//   weapon_gen --weapon sword --force       # regenerate just sword
//   weapon_gen --dir path/to/meshes         # custom output directory
//   weapon_gen --list                       # list all built-in weapons
//   weapon_gen --icons                      # preview all icon bitmaps as ASCII art
//   weapon_gen --icons-src <path>           # write icon source code to file

static void printUsage() {
    std::printf("Usage: weapon_gen [options]\n");
    std::printf("\n  Mesh generation:\n");
    std::printf("  --force              Overwrite existing OBJ files\n");
    std::printf("  --weapon <name>      Generate only the named weapon\n");
    std::printf("  --dir <path>         Output directory (default: assets/meshes)\n");
    std::printf("  --list               List all built-in mesh definitions\n");
    std::printf("\n  Icon tools:\n");
    std::printf("  --icons              Preview all icon bitmaps as ASCII art\n");
    std::printf("  --icons-src <path>   Write icon C source snippet to file\n");
    std::printf("\n  --help               Show this help\n");
}

int main(int argc, char* argv[]) {
    const char* meshDir = "assets/meshes";
    const char* singleWeapon = nullptr;
    const char* iconsSrcPath = nullptr;
    bool force = false;
    bool listMode = false;
    bool iconsMode = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            meshDir = argv[++i];
        } else if (std::strcmp(argv[i], "--weapon") == 0 && i + 1 < argc) {
            singleWeapon = argv[++i];
        } else if (std::strcmp(argv[i], "--list") == 0) {
            listMode = true;
        } else if (std::strcmp(argv[i], "--icons") == 0) {
            iconsMode = true;
        } else if (std::strcmp(argv[i], "--icons-src") == 0 && i + 1 < argc) {
            iconsSrcPath = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printUsage();
            return 0;
        } else {
            std::printf("Unknown option: %s\n", argv[i]);
            printUsage();
            return 1;
        }
    }

    // Icon source export mode
    if (iconsSrcPath) {
        if (WeaponModelGen::writeIconSource(iconsSrcPath)) {
            std::printf("Wrote icon source to %s\n", iconsSrcPath);
        } else {
            std::printf("Error: could not write to %s\n", iconsSrcPath);
            return 1;
        }
        return 0;
    }

    // Icon ASCII preview mode
    if (iconsMode) {
        u32 count;
        const auto* icons = WeaponModelGen::getAllIcons(count);
        std::printf("Built-in icon bitmaps (%u):\n\n", count);
        for (u32 i = 0; i < count; i++) {
            WeaponModelGen::previewIcon(icons[i]);
            std::printf("\n");
        }
        return 0;
    }

    // List mode: print all mesh definitions
    if (listMode) {
        u32 count;
        const auto* defs = WeaponModelGen::getAllDefs(count);
        std::printf("Built-in mesh definitions (%u):\n", count);
        for (u32 i = 0; i < count; i++) {
            std::printf("  %-16s  %u boxes  %s\n",
                        defs[i].name, defs[i].boxCount,
                        defs[i].comment ? defs[i].comment : "");
        }
        return 0;
    }

    // Single weapon mode
    if (singleWeapon) {
        const auto* def = WeaponModelGen::getBuiltinDef(singleWeapon);
        if (!def) {
            std::printf("Error: unknown weapon '%s'\n", singleWeapon);
            std::printf("Use --list to see available weapons.\n");
            return 1;
        }
        if (WeaponModelGen::generateWeapon(meshDir, *def, force)) {
            std::printf("Generated: %s/%s.obj\n", meshDir, def->name);
        } else {
            std::printf("Skipped: %s/%s.obj (already exists, use --force)\n",
                        meshDir, def->name);
        }
        return 0;
    }

    // Generate all meshes
    u32 written = WeaponModelGen::generateAll(meshDir, force);
    u32 total;
    WeaponModelGen::getAllDefs(total);
    std::printf("Generated %u/%u OBJ files in %s/\n", written, total, meshDir);

    return 0;
}
