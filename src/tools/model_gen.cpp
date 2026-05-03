#include "tools/model_gen.h"
#include <cstdio>
#include <cstring>

// ============================================================
//  Built-in weapon definitions (box dimensions from existing OBJs)
// ============================================================

static const WeaponModelGen::WeaponDef s_builtinDefs[] = {
    // ---- MELEE ----
    {
        "sword", "Elongated blade + handle",
        {
            {{-0.025f, -0.15f, -0.01f},  {0.025f,  0.45f,  0.01f}},   // blade
            {{-0.02f,  -0.275f, -0.02f}, {0.02f,  -0.125f, 0.02f}},   // handle
        }, 2
    },
    {
        "dagger", "Short blade + crossguard",
        {
            {{-0.02f,  -0.10f,  -0.01f},  {0.02f,   0.20f,  0.01f}},  // blade
            {{-0.0175f,-0.24f,  -0.0175f},{0.0175f, -0.12f,  0.0175f}},// handle
        }, 2
    },
    {
        "axe", "Asymmetric axe blade on shaft",
        {
            {{-0.12f,  0.15f,  -0.015f}, {0.0f,   0.30f,  0.015f}},   // blade (extends left only)
            {{ 0.0f,   0.18f,  -0.02f},  {0.04f,  0.26f,  0.02f}},    // poll (small block opposite)
            {{-0.015f,-0.30f,  -0.015f}, {0.015f,  0.18f,  0.015f}},   // shaft
        }, 3
    },

    // ---- HITSCAN ----
    {
        "pistol", "L-shaped handgun",
        {
            {{-0.02f,  0.0f,   -0.05f},  {0.02f,  0.04f,  0.15f}},   // barrel
            {{-0.02f, -0.12f,  -0.065f},  {0.02f,  0.0f,  -0.035f}},  // grip
        }, 2
    },
    {
        "smg", "Longer L-shape with magazine",
        {
            {{-0.02f,   0.0f,  -0.05f},  {0.02f,   0.05f,  0.25f}},  // barrel
            {{-0.02f,  -0.12f, -0.035f},  {0.02f,   0.0f,  -0.005f}}, // grip
            {{-0.015f, -0.08f,  0.01f},   {0.015f,  0.0f,   0.03f}},  // magazine
        }, 3
    },
    {
        "carbine", "Long rifle profile",
        {
            {{-0.0175f, 0.0f,  -0.075f},  {0.0175f, 0.04f,  0.375f}}, // barrel
            {{-0.02f,  -0.025f,-0.225f},   {0.02f,   0.025f,-0.075f}}, // stock
            {{-0.0175f,-0.10f, -0.035f},   {0.0175f, 0.0f,  -0.005f}}, // grip
        }, 3
    },
    {
        "revolver", "Rounded cylinder + barrel",
        {
            {{-0.02f,   0.0f,  -0.025f},  {0.02f,  0.04f,  0.125f}},  // barrel
            {{-0.03f,  -0.01f, -0.04f},   {0.03f,  0.05f,  0.02f}},   // cylinder
            {{-0.02f,  -0.13f, -0.055f},   {0.02f,  0.0f,  -0.025f}}, // grip
        }, 3
    },

    // ---- PROJECTILE ----
    {
        "bow", "Curved arc with grip",
        {
            {{-0.015f, -0.03f, -0.015f}, {0.015f, 0.03f,  0.015f}},  // grip
            {{-0.04f,   0.03f, -0.01f},  {-0.02f, 0.23f,  0.01f}},   // upper limb
            {{-0.04f,  -0.23f, -0.01f},  {-0.02f,-0.03f,  0.01f}},   // lower limb
        }, 3
    },
    {
        "crossbow", "Crossbow with limbs, groove, and trigger grip",
        {
            {{-0.015f, -0.015f, -0.10f},  {0.015f, 0.015f, 0.18f}},  // stock/tiller
            {{-0.12f,   0.01f,   0.12f},  {-0.02f, 0.03f,  0.16f}},  // left limb (angled)
            {{ 0.02f,   0.01f,   0.12f},  {0.12f,  0.03f,  0.16f}},  // right limb (angled)
            {{-0.008f,  0.015f, -0.02f},  {0.008f, 0.025f, 0.14f}},  // flight groove (rail on top)
            {{-0.02f,  -0.08f, -0.06f},  {0.02f, -0.015f, -0.02f}},  // trigger grip
        }, 5
    },
    {
        "throwing_knife", "Tiny blade + handle",
        {
            {{-0.01f,  -0.045f, -0.0025f},{0.01f,  0.105f,  0.0025f}},// blade
            {{-0.0075f,-0.12f,  -0.0075f},{0.0075f,-0.04f,  0.0075f}},// handle
        }, 2
    },
    {
        "molotov", "Bottle with neck",
        {
            {{-0.03f,  -0.07f, -0.03f},  {0.03f,  0.05f,  0.03f}},   // body
            {{-0.015f,  0.06f, -0.015f},  {0.015f, 0.12f,  0.015f}},  // neck
        }, 2
    },
    {
        "wand", "Magic wand with crystal tip",
        {
            {{-0.015f, -0.25f, -0.015f}, {0.015f,  0.20f,  0.015f}},  // shaft (thin stick)
            {{-0.025f,  0.20f, -0.025f}, {0.025f,  0.30f,  0.025f}},  // crystal head
            {{-0.02f,  -0.28f, -0.02f},  {0.02f,  -0.22f,  0.02f}},   // pommel
        }, 3
    },

    {
        "cleaver", "Large butcher cleaver",
        {
            {{-0.15f,  0.10f,  -0.01f}, {0.02f,   0.35f,  0.01f}},   // wide blade
            {{-0.02f, -0.20f,  -0.02f}, {0.02f,   0.12f,  0.02f}},   // handle
            {{-0.03f,  0.08f,  -0.015f},{0.03f,   0.12f,  0.015f}},  // guard
        }, 3
    },
    {
        "mace", "Flanged mace with shaft",
        {
            {{-0.015f, -0.25f, -0.015f}, {0.015f,  0.15f,  0.015f}},  // shaft
            {{-0.04f,   0.15f, -0.04f},  {0.04f,   0.25f,  0.04f}},   // head (cube)
            {{-0.05f,   0.17f, -0.01f},  {0.05f,   0.23f,  0.01f}},   // flange X
            {{-0.01f,   0.17f, -0.05f},  {0.01f,   0.23f,  0.05f}},   // flange Z
        }, 4
    },

    {
        "iron_maiden", "Iron maiden torture device",
        {
            {{-0.15f, 0.0f,  -0.12f}, {0.15f, 0.90f, 0.12f}},   // main body (coffin shape)
            {{-0.17f, 0.0f,  -0.14f}, {-0.15f, 0.90f, 0.14f}},  // left hinge side
            {{ 0.15f, 0.0f,  -0.14f}, { 0.17f, 0.90f, 0.14f}},  // right hinge side
            {{-0.15f, 0.90f, -0.12f}, {0.15f, 0.95f, 0.12f}},   // top cap
            {{-0.10f, 0.30f, -0.13f}, {0.10f, 0.70f, -0.12f}},  // front face (slightly protruding)
        }, 5
    },

    // ---- ARMOR / EQUIPMENT ----
    {
        "helmet", "Dome helm with brim",
        {
            {{-0.12f,  0.0f,  -0.12f},  {0.12f,  0.18f,  0.12f}},   // dome
            {{-0.14f, -0.03f, -0.14f},  {0.14f,  0.02f,  0.14f}},   // brim
            {{-0.06f, -0.08f,  0.08f},  {0.06f,  0.0f,   0.14f}},   // nose guard
        }, 3
    },
    {
        "armor", "Breastplate with shoulders",
        {
            {{-0.12f, -0.20f, -0.06f},  {0.12f,  0.15f,  0.06f}},   // torso
            {{-0.20f,  0.08f, -0.05f},  {-0.12f, 0.18f,  0.05f}},   // left shoulder
            {{ 0.12f,  0.08f, -0.05f},  {0.20f,  0.18f,  0.05f}},   // right shoulder
            {{-0.08f, -0.22f, -0.04f},  {0.08f, -0.18f,  0.04f}},   // belt
        }, 4
    },
    {
        "boots", "Ankle boot with sole",
        {
            {{-0.06f,  0.0f,  -0.04f},  {0.06f,  0.14f,  0.04f}},   // shaft
            {{-0.07f, -0.03f, -0.05f},  {0.07f,  0.0f,   0.10f}},   // sole + toe
        }, 2
    },
    {
        "ring", "Band with gemstone",
        {
            {{-0.08f, -0.02f, -0.08f},  {0.08f,  0.02f,  0.08f}},   // band (flat wide)
            {{-0.03f,  0.02f, -0.03f},  {0.03f,  0.07f,  0.03f}},   // gem
        }, 2
    },
    {
        "shield", "Kite shield",
        {
            {{-0.12f, -0.18f, -0.02f},  {0.12f,  0.18f,  0.02f}},   // main panel
            {{-0.04f, -0.05f,  0.02f},  {0.04f,  0.05f,  0.05f}},   // grip/boss
        }, 2
    },
};

static constexpr u32 BUILTIN_COUNT = sizeof(s_builtinDefs) / sizeof(s_builtinDefs[0]);

// ============================================================
//  OBJ writer
// ============================================================

bool WeaponModelGen::writeOBJ(const char* path, const BoxDef* boxes, u32 boxCount,
                               const char* comment) {
    if (!path || !boxes || boxCount == 0) return false;

    FILE* f = std::fopen(path, "w");
    if (!f) return false;

    // Header comment
    if (comment) {
        std::fprintf(f, "# %s\n", comment);
    }
    std::fprintf(f, "# Generated by WeaponModelGen (%u boxes)\n\n", boxCount);

    // Write vertices for each box (8 per box)
    // Vertex order: the 8 corners of an AABB
    //   0: min.x, min.y, min.z    4: min.x, min.y, max.z
    //   1: max.x, min.y, min.z    5: max.x, min.y, max.z
    //   2: max.x, max.y, min.z    6: max.x, max.y, max.z
    //   3: min.x, max.y, min.z    7: min.x, max.y, max.z
    for (u32 b = 0; b < boxCount; b++) {
        const BoxDef& box = boxes[b];
        f32 x0 = box.min.x, y0 = box.min.y, z0 = box.min.z;
        f32 x1 = box.max.x, y1 = box.max.y, z1 = box.max.z;

        std::fprintf(f, "# --- Box %u vertices ---\n", b);
        std::fprintf(f, "v %g %g %g\n", x0, y0, z0);  // 0
        std::fprintf(f, "v %g %g %g\n", x1, y0, z0);  // 1
        std::fprintf(f, "v %g %g %g\n", x1, y1, z0);  // 2
        std::fprintf(f, "v %g %g %g\n", x0, y1, z0);  // 3
        std::fprintf(f, "v %g %g %g\n", x0, y0, z1);  // 4
        std::fprintf(f, "v %g %g %g\n", x1, y0, z1);  // 5
        std::fprintf(f, "v %g %g %g\n", x1, y1, z1);  // 6
        std::fprintf(f, "v %g %g %g\n", x0, y1, z1);  // 7
        std::fprintf(f, "\n");
    }

    // Shared normals (6 axis-aligned, same for all boxes)
    std::fprintf(f, "# --- Normals ---\n");
    std::fprintf(f, "vn  0.0  0.0 -1.0\n");  // 1: -Z
    std::fprintf(f, "vn  0.0  0.0  1.0\n");  // 2: +Z
    std::fprintf(f, "vn -1.0  0.0  0.0\n");  // 3: -X
    std::fprintf(f, "vn  1.0  0.0  0.0\n");  // 4: +X
    std::fprintf(f, "vn  0.0 -1.0  0.0\n");  // 5: -Y
    std::fprintf(f, "vn  0.0  1.0  0.0\n");  // 6: +Y
    std::fprintf(f, "\n");

    // Shared UVs (4 corners)
    std::fprintf(f, "# --- UVs ---\n");
    std::fprintf(f, "vt 0.0 0.0\n");  // 1
    std::fprintf(f, "vt 1.0 0.0\n");  // 2
    std::fprintf(f, "vt 1.0 1.0\n");  // 3
    std::fprintf(f, "vt 0.0 1.0\n");  // 4
    std::fprintf(f, "\n");

    // Faces: 12 triangles per box, matching the winding from existing OBJ files
    for (u32 b = 0; b < boxCount; b++) {
        u32 v = b * 8 + 1; // 1-based vertex offset for this box

        std::fprintf(f, "# === Box %u faces ===\n", b);

        // -Z face (vn 1)
        std::fprintf(f, "f %u/4/1 %u/3/1 %u/2/1\n", v+3, v+2, v+1);
        std::fprintf(f, "f %u/4/1 %u/2/1 %u/1/1\n", v+3, v+1, v+0);
        // +Z face (vn 2)
        std::fprintf(f, "f %u/1/2 %u/2/2 %u/3/2\n", v+4, v+5, v+6);
        std::fprintf(f, "f %u/1/2 %u/3/2 %u/4/2\n", v+4, v+6, v+7);
        // -X face (vn 3)
        std::fprintf(f, "f %u/1/3 %u/2/3 %u/3/3\n", v+0, v+4, v+7);
        std::fprintf(f, "f %u/1/3 %u/3/3 %u/4/3\n", v+0, v+7, v+3);
        // +X face (vn 4)
        std::fprintf(f, "f %u/1/4 %u/2/4 %u/3/4\n", v+5, v+1, v+2);
        std::fprintf(f, "f %u/1/4 %u/3/4 %u/4/4\n", v+5, v+2, v+6);
        // -Y face (vn 5)
        std::fprintf(f, "f %u/1/5 %u/2/5 %u/3/5\n", v+1, v+5, v+4);
        std::fprintf(f, "f %u/1/5 %u/3/5 %u/4/5\n", v+1, v+4, v+0);
        // +Y face (vn 6)
        std::fprintf(f, "f %u/1/6 %u/2/6 %u/3/6\n", v+7, v+6, v+2);
        std::fprintf(f, "f %u/1/6 %u/3/6 %u/4/6\n", v+7, v+2, v+3);

        std::fprintf(f, "\n");
    }

    std::fclose(f);
    return true;
}

// ============================================================
//  High-level generation helpers
// ============================================================

bool WeaponModelGen::generateWeapon(const char* meshDir, const WeaponDef& def, bool force) {
    char path[256];
    std::snprintf(path, sizeof(path), "%s/%s.obj", meshDir, def.name);

    // Skip if file exists (unless forced)
    if (!force) {
        FILE* test = std::fopen(path, "r");
        if (test) {
            std::fclose(test);
            return false;
        }
    }

    return writeOBJ(path, def.boxes, def.boxCount, def.comment);
}

u32 WeaponModelGen::generateAll(const char* meshDir, bool force) {
    u32 written = 0;
    for (u32 i = 0; i < BUILTIN_COUNT; i++) {
        if (generateWeapon(meshDir, s_builtinDefs[i], force)) {
            written++;
        }
    }
    return written;
}

const WeaponModelGen::WeaponDef* WeaponModelGen::getBuiltinDef(const char* name) {
    for (u32 i = 0; i < BUILTIN_COUNT; i++) {
        if (std::strcmp(s_builtinDefs[i].name, name) == 0)
            return &s_builtinDefs[i];
    }
    return nullptr;
}

const WeaponModelGen::WeaponDef* WeaponModelGen::getAllDefs(u32& outCount) {
    outCount = BUILTIN_COUNT;
    return s_builtinDefs;
}

// ============================================================
//  Icon bitmap data — mirrors item_icons.cpp s_iconData[]
//  Agent-friendly: modify bitmaps here, run --icons to preview
// ============================================================

static const WeaponModelGen::IconDef s_builtinIcons[] = {
    {"none",           {0x0000,0x0000,0x0000,0x0000,0x0180,0x03C0,0x07E0,0x0FF0,
                        0x0FF0,0x07E0,0x03C0,0x0180,0x0000,0x0000,0x0000,0x0000}},
    {"sword",          {0x0180,0x0180,0x0180,0x0180,0x01C0,0x01C0,0x03C0,0x03C0,
                        0x0180,0x0FF0,0x0FF0,0x0180,0x0180,0x0180,0x03C0,0x07E0}},
    {"dagger",         {0x0000,0x0000,0x0180,0x0180,0x0180,0x01C0,0x01C0,0x0180,
                        0x0FF0,0x0FF0,0x0180,0x0180,0x0180,0x03C0,0x0000,0x0000}},
    {"axe",            {0x0000,0x0F00,0x1F80,0x3FC0,0x3FC0,0x1F80,0x0F80,0x0180,
                        0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x03C0,0x03C0}},
    {"pistol",         {0x0000,0x0000,0x0000,0x7FC0,0x7FE0,0x7FE0,0x01E0,0x01E0,
                        0x03E0,0x07C0,0x0FC0,0x0FC0,0x0F80,0x0F00,0x0000,0x0000}},
    {"smg",            {0x0000,0x0000,0xFFC0,0xFFE0,0xFFE0,0xFFE0,0x03E0,0x03E0,
                        0x07E0,0x0FC0,0x0FC0,0x0F80,0x0F80,0x0F00,0x0000,0x0000}},
    {"carbine",        {0x0000,0xFFF0,0xFFF8,0xFFF8,0x00F8,0x00F0,0x01F0,0x03E0,
                        0x07C0,0x0F80,0x0F80,0x0F00,0x0F00,0x0E00,0x0000,0x0000}},
    {"revolver",       {0x0000,0x0000,0x7F00,0x7F80,0x7FC0,0x7FC0,0x07C0,0x0FC0,
                        0x1F80,0x1F80,0x3F00,0x3F00,0x3E00,0x0000,0x0000,0x0000}},
    {"bow",            {0x0080,0x0180,0x0280,0x0480,0x0880,0x1080,0x2080,0x4080,
                        0x4080,0x2080,0x1080,0x0880,0x0480,0x0280,0x0180,0x0080}},
    {"crossbow",       {0x0000,0xFFFF,0xFFFF,0x8181,0xC3C3,0x6666,0x0180,0x0180,
                        0x0180,0x0180,0x0180,0x0180,0x03C0,0x07E0,0x0000,0x0000}},
    {"throwing_knife", {0x0000,0x0003,0x0006,0x000C,0x0018,0x0030,0x0060,0x00C0,
                        0x0180,0x0380,0x0700,0x0E00,0x0C00,0x1800,0x1000,0x0000}},
    {"molotov",        {0x0100,0x0380,0x0280,0x0100,0x0180,0x0180,0x0180,0x03C0,
                        0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x03C0,0x03C0}},
    {"shield",         {0x0000,0x3FFC,0x7FFE,0x7FFE,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
                        0x7FFE,0x7FFE,0x3FFC,0x1FF8,0x0FF0,0x07E0,0x03C0,0x0180}},
    {"helmet",         {0x0000,0x0000,0x07E0,0x0FF0,0x1FF8,0x3FFC,0x3FFC,0x7FFE,
                        0x7FFE,0x7FFE,0xFFFF,0xFFFF,0xFFFF,0x0000,0x0000,0x0000}},
    {"armor",          {0x0000,0xE00E,0xF01E,0x783C,0x3FF8,0x1FF0,0x1FF0,0x1FF0,
                        0x1FF0,0x1FF0,0x1FF0,0x1FF0,0x0FE0,0x0FE0,0x07C0,0x0000}},
    {"boots",          {0x0000,0x0000,0x0F00,0x0F00,0x0F00,0x0F00,0x0F00,0x0F00,
                        0x0F00,0x0F00,0x0F80,0x0FC0,0x0FE0,0x1FF0,0x1FF0,0x0000}},
    {"ring",           {0x0000,0x0000,0x0000,0x03C0,0x07E0,0x0E70,0x0C30,0x1818,
                        0x1818,0x0C30,0x0E70,0x07E0,0x03C0,0x0000,0x0000,0x0000}},
};

static constexpr u32 ICON_COUNT = sizeof(s_builtinIcons) / sizeof(s_builtinIcons[0]);

const WeaponModelGen::IconDef* WeaponModelGen::getAllIcons(u32& outCount) {
    outCount = ICON_COUNT;
    return s_builtinIcons;
}

void WeaponModelGen::previewIcon(const IconDef& icon) {
    std::printf("  %s:\n", icon.name);
    for (u32 y = 0; y < 16; y++) {
        std::printf("    ");
        for (u32 x = 0; x < 16; x++) {
            bool on = (icon.rows[y] >> (15 - x)) & 1;
            std::printf("%s", on ? "##" : "  ");
        }
        std::printf("\n");
    }
}

bool WeaponModelGen::writeIconSource(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return false;

    std::fprintf(f, "// Auto-generated icon bitmap data\n");
    std::fprintf(f, "// Copy into item_icons.cpp s_iconData[] array\n\n");

    for (u32 i = 0; i < ICON_COUNT; i++) {
        const IconDef& icon = s_builtinIcons[i];
        std::fprintf(f, "    // %u: %s\n    {\n        ", i, icon.name);
        for (u32 r = 0; r < 16; r++) {
            std::fprintf(f, "0x%04X,", icon.rows[r]);
            if (r == 3 || r == 7 || r == 11) std::fprintf(f, "\n        ");
            else if (r < 15) std::fprintf(f, " ");
        }
        std::fprintf(f, "\n    },\n");
    }

    std::fclose(f);
    return true;
}
