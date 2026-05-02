#include "world/level_loader.h"
#include "renderer/material.h"
#include "core/log.h"

#include <cstdio>
#include <cstring>
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;

Vec3 LevelLoader::loadFromJson(const char* path, LevelGrid& grid,
                                EnemySpawn* outSpawns, u32& outSpawnCount, u32 maxSpawns)
{
    outSpawnCount = 0;

    FILE* f = std::fopen(path, "r");
    if (!f) {
        LOG_WARN("LevelLoader: could not open %s", path);
        return {0, 0, 0};
    }

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    char* buf = static_cast<char*>(std::malloc(size + 1));
    std::fread(buf, 1, size, f);
    buf[size] = '\0';
    std::fclose(f);

    Vec3 spawnPos = {0, 0, 0};

    try {
        json doc = json::parse(buf);
        std::free(buf);

        u32 w = doc.value("width", 32u);
        u32 d = doc.value("depth", 32u);
        f32 cs = doc.value("cellSize", 1.0f);
        f32 defaultCeil = doc.value("defaultCeiling", 3.0f);

        // Reinit grid if dimensions differ
        if (grid.width != w || grid.depth != d) {
            LevelGridSystem::shutdown(grid);
            LevelGridSystem::init(grid, w, d, cs);
        }

        // Fill all solid
        for (u32 z = 0; z < grid.depth; z++) {
            for (u32 x = 0; x < grid.width; x++) {
                GridCell& c = LevelGridSystem::getCell(grid, x, z);
                c.flags           = CELL_SOLID;
                c.floorHeight     = 0;
                c.ceilingHeight   = 0;
                c.wallMaterialId  = 0;
                c.floorMaterialId = 0;
                c.ceilMaterialId  = 0;
            }
        }

        // Carve rooms
        if (doc.contains("rooms") && doc["rooms"].is_array()) {
            for (auto& room : doc["rooms"]) {
                u32 rx = room.value("x", 0u);
                u32 rz = room.value("z", 0u);
                u32 rw = room.value("w", 4u);
                u32 rd = room.value("d", 4u);
                f32 floorH = room.value("floorHeight", 0.0f);
                f32 ceilH  = room.value("ceilingHeight", defaultCeil);

                // Material names → IDs
                u8 wallMat  = 0;
                u8 floorMat = 1;
                u8 ceilMat  = 2;
                if (room.contains("wallMaterial"))
                    wallMat = MaterialSystem::getIdByName(room["wallMaterial"].get<std::string>().c_str());
                if (room.contains("floorMaterial"))
                    floorMat = MaterialSystem::getIdByName(room["floorMaterial"].get<std::string>().c_str());
                if (room.contains("ceilMaterial"))
                    ceilMat = MaterialSystem::getIdByName(room["ceilMaterial"].get<std::string>().c_str());

                u8 floorEnc = static_cast<u8>(floorH / 0.25f);
                u8 ceilEnc  = static_cast<u8>(ceilH / 0.25f);

                for (u32 z = rz; z < rz + rd; z++) {
                    for (u32 x = rx; x < rx + rw; x++) {
                        if (!LevelGridSystem::isInBounds(grid, x, z)) continue;
                        GridCell& cell = LevelGridSystem::getCell(grid, x, z);
                        cell.flags           = CELL_FLOOR | CELL_CEILING;
                        cell.floorHeight     = floorEnc;
                        cell.ceilingHeight   = ceilEnc;
                        cell.wallMaterialId  = wallMat;
                        cell.floorMaterialId = floorMat;
                        cell.ceilMaterialId  = ceilMat;
                    }
                }
            }
        }

        // Carve corridors
        if (doc.contains("corridors") && doc["corridors"].is_array()) {
            for (auto& corr : doc["corridors"]) {
                u32 x0 = corr.value("x0", 0u);
                u32 z0 = corr.value("z0", 0u);
                u32 x1 = corr.value("x1", 0u);
                u32 z1 = corr.value("z1", 0u);
                f32 floorH = corr.value("floorHeight", 0.0f);
                f32 ceilH  = corr.value("ceilingHeight", defaultCeil);

                u8 floorEnc = static_cast<u8>(floorH / 0.25f);
                u8 ceilEnc  = static_cast<u8>(ceilH / 0.25f);

                // Horizontal leg then vertical leg (L-shaped)
                u32 xMin = (x0 < x1) ? x0 : x1;
                u32 xMax = (x0 > x1) ? x0 : x1;
                for (u32 x = xMin; x <= xMax; x++) {
                    for (s32 dz = -1; dz <= 0; dz++) {
                        s32 cz = (s32)z0 + dz;
                        if (cz < 0) continue;
                        if (!LevelGridSystem::isInBounds(grid, x, (u32)cz)) continue;
                        GridCell& cell = LevelGridSystem::getCell(grid, x, (u32)cz);
                        if (cell.flags & CELL_SOLID) {
                            cell.flags           = CELL_FLOOR | CELL_CEILING;
                            cell.floorHeight     = floorEnc;
                            cell.ceilingHeight   = ceilEnc;
                            cell.wallMaterialId  = 0;
                            cell.floorMaterialId = 1;
                            cell.ceilMaterialId  = 2;
                        }
                    }
                }

                u32 zMin = (z0 < z1) ? z0 : z1;
                u32 zMax = (z0 > z1) ? z0 : z1;
                for (u32 z = zMin; z <= zMax; z++) {
                    for (s32 dx = -1; dx <= 0; dx++) {
                        s32 cx = (s32)x1 + dx;
                        if (cx < 0) continue;
                        if (!LevelGridSystem::isInBounds(grid, (u32)cx, z)) continue;
                        GridCell& cell = LevelGridSystem::getCell(grid, (u32)cx, z);
                        if (cell.flags & CELL_SOLID) {
                            cell.flags           = CELL_FLOOR | CELL_CEILING;
                            cell.floorHeight     = floorEnc;
                            cell.ceilingHeight   = ceilEnc;
                            cell.wallMaterialId  = 0;
                            cell.floorMaterialId = 1;
                            cell.ceilMaterialId  = 2;
                        }
                    }
                }
            }
        }

        // Player spawn
        if (doc.contains("spawns") && doc["spawns"].contains("player")) {
            auto& ps = doc["spawns"]["player"];
            if (ps.contains("x") && ps.contains("z")) {
                spawnPos.x = ps["x"].get<f32>();
                spawnPos.z = ps["z"].get<f32>();
            } else if (ps.contains("room") && doc.contains("rooms")) {
                u32 roomIdx = ps["room"].get<u32>();
                if (roomIdx < doc["rooms"].size()) {
                    auto& room = doc["rooms"][roomIdx];
                    spawnPos.x = (room.value("x", 0u) + room.value("w", 4u) * 0.5f) * grid.cellSize;
                    spawnPos.z = (room.value("z", 0u) + room.value("d", 4u) * 0.5f) * grid.cellSize;
                }
            }
        }

        // Enemy spawns
        if (doc.contains("spawns") && doc["spawns"].contains("enemies")) {
            for (auto& es : doc["spawns"]["enemies"]) {
                if (outSpawnCount >= maxSpawns) break;
                EnemySpawn& s = outSpawns[outSpawnCount++];
                std::string name = es.value("type", "skeleton");
                std::strncpy(s.type, name.c_str(), sizeof(s.type) - 1);
                s.x = es.value("x", 0.0f);
                s.z = es.value("z", 0.0f);
            }
        }

        LOG_INFO("LevelLoader: loaded %s (%ux%u, %u enemy spawns)", path, w, d, outSpawnCount);

    } catch (const json::exception& e) {
        std::free(buf);
        LOG_WARN("LevelLoader: JSON error in %s: %s", path, e.what());
        return {0, 0, 0};
    }

    return spawnPos;
}
