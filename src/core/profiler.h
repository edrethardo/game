#pragma once

#include "core/types.h"
#include <SDL.h>

static constexpr u32 PROFILER_MAX_SCOPES = 16;
static constexpr u32 PROFILER_HISTORY    = 120; // 2 seconds at 60 FPS

struct ProfileScope {
    const char* name    = nullptr;
    f64         elapsedMs = 0.0;
};

struct Profiler {
    ProfileScope scopes[PROFILER_MAX_SCOPES];
    u32          scopeCount = 0;
    bool         enabled    = false;

    // Frame time history
    f64 frameTimeHistory[PROFILER_HISTORY] = {};
    u32 historyHead = 0;
    u32 historyCount = 0;

    f64 frameTimeMin = 0.0;
    f64 frameTimeMax = 0.0;
    f64 frameTimeAvg = 0.0;
};

// Global profiler instance
inline Profiler& getProfiler() {
    static Profiler s_profiler;
    return s_profiler;
}

// RAII scope timer
struct ScopedTimer {
    u32 scopeIndex;
    u64 start;

    ScopedTimer(u32 idx, const char* name) : scopeIndex(idx) {
        Profiler& p = getProfiler();
        if (scopeIndex >= PROFILER_MAX_SCOPES) return;
        p.scopes[scopeIndex].name = name;
        if (scopeIndex >= p.scopeCount) p.scopeCount = scopeIndex + 1;
        start = SDL_GetPerformanceCounter();
    }

    ~ScopedTimer() {
        if (scopeIndex >= PROFILER_MAX_SCOPES) return;
        u64 end = SDL_GetPerformanceCounter();
        f64 freq = static_cast<f64>(SDL_GetPerformanceFrequency());
        getProfiler().scopes[scopeIndex].elapsedMs = (end - start) / freq * 1000.0;
    }
};

inline void profilerRecordFrame(f64 frameMs) {
    Profiler& p = getProfiler();
    p.frameTimeHistory[p.historyHead] = frameMs;
    p.historyHead = (p.historyHead + 1) % PROFILER_HISTORY;
    if (p.historyCount < PROFILER_HISTORY) p.historyCount++;

    // Compute min/max/avg over history
    f64 mn = 1e9, mx = 0.0, sum = 0.0;
    for (u32 i = 0; i < p.historyCount; i++) {
        f64 t = p.frameTimeHistory[i];
        if (t < mn) mn = t;
        if (t > mx) mx = t;
        sum += t;
    }
    p.frameTimeMin = mn;
    p.frameTimeMax = mx;
    p.frameTimeAvg = sum / p.historyCount;
}

#define PROFILE_SCOPE(idx, name) ScopedTimer _profTimer##idx(idx, name)
