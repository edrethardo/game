// floor_event_loader.cpp — parses assets/config/events.json into a FloorEventTable.
// Split from floor_event.cpp so the pure selection logic can be unit-tested without pulling
// nlohmann/json into the test binary.

#include "game/floor_event_loader.h"
#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;

bool FloorEventLoader::load(const char* path, FloorEventTable& out) {
    out = FloorEventTable{};

    FILE* f = std::fopen(path, "r");
    if (!f) {
        LOG_WARN("FloorEvent: %s not found — floor events disabled", path);
        return false;   // no events is a valid game, so this is not fatal
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1));
    if (!buf) { std::fclose(f); return false; }
    (void)std::fread(buf, 1, static_cast<size_t>(size), f);
    buf[size] = '\0';
    std::fclose(f);

    try {
        json j = json::parse(buf);
        std::free(buf);

        out.floorChance = j.value("floorChance", 0.35f);

        if (j.contains("events")) {
            for (const auto& e : j["events"]) {
                if (out.count >= MAX_FLOOR_EVENT_DEFS) {
                    LOG_WARN("FloorEvent: more than %u events — ignoring the rest",
                             MAX_FLOOR_EVENT_DEFS);
                    break;
                }
                const std::string idStr = e.value("id", std::string(""));
                const FloorEventId id = FloorEvent::idFromName(idStr.c_str());
                if (id == FloorEventId::NONE) {
                    // An id with no spawner would otherwise be a silent no-op event — the exact
                    // "ships, looks live, does nothing" failure this codebase keeps producing. Say so.
                    LOG_WARN("FloorEvent: unknown event id '%s' — skipped", idStr.c_str());
                    continue;
                }
                FloorEventDef& d = out.defs[out.count++];
                d.id       = id;
                d.weight   = static_cast<u8>(e.value("weight", 0));
                d.minFloor = static_cast<u8>(e.value("minFloor", 1));
            }
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("FloorEvent: failed to parse %s: %s", path, ex.what());
        return false;
    }

    LOG_INFO("FloorEvent: loaded %u event(s), floorChance %.2f",
             out.count, static_cast<f64>(out.floorChance));
    return true;
}
