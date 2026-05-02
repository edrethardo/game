#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "platform/clock.h"

static u64 s_frequency = 0;
static u64 s_startCounter = 0;
static u64 s_lastCounter = 0;
static f64 s_deltaSeconds = 0.0;
static f64 s_elapsedSeconds = 0.0;
static u64 s_frameCount = 0;

void Clock::init() {
    s_frequency = SDL_GetPerformanceFrequency();
    s_startCounter = SDL_GetPerformanceCounter();
    s_lastCounter = s_startCounter;
    s_deltaSeconds = 0.0;
    s_elapsedSeconds = 0.0;
    s_frameCount = 0;
}

void Clock::update() {
    u64 now = SDL_GetPerformanceCounter();
    s_deltaSeconds = static_cast<f64>(now - s_lastCounter) / static_cast<f64>(s_frequency);
    s_lastCounter = now;
    s_elapsedSeconds = static_cast<f64>(now - s_startCounter) / static_cast<f64>(s_frequency);
    s_frameCount++;
}

f64 Clock::getDeltaSeconds() {
    return s_deltaSeconds;
}

f64 Clock::getElapsedSeconds() {
    return s_elapsedSeconds;
}

u64 Clock::getFrameCount() {
    return s_frameCount;
}
