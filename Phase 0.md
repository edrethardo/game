# Phase 0 — Technical Foundation (Detailed, Iterable Plan)

## Objective

Establish a deterministic, low-overhead runtime foundation that supports:

* Stable 60 FPS simulation
* Zero per-frame allocations
* Clean platform abstraction (PC → Switch-ready)

This phase must result in a **fully stable execution loop** before any rendering or gameplay is built.

---

# Success Criteria (Phase Gate)

You do NOT proceed unless:

* Fixed timestep runs without drift for 10+ minutes
* Input latency ≤ 1 frame
* Frame time variance < 0.5 ms (idle)
* Zero allocations during runtime loop
* All systems measurable via profiling hooks

---

# Subphase 0.1 — Platform Layer

## Goal

Abstract OS-specific functionality while staying minimal and predictable.

---

## Tasks

### 0.1.1 Window System

* Initialize SDL2
* Create window (no OpenGL yet, or minimal context)
* Handle resize and close events

### 0.1.2 Input System

* Keyboard (WASD, ESC, debug keys)

* Mouse:

  * Relative mode (for FPS camera later)
  * Absolute mode (for UI later)

* Controller (basic abstraction only)

  * Axes
  * Buttons

### 0.1.3 Timing System

* High-resolution timer (SDL or platform-specific)
* Functions:

  * `GetTime()`
  * `GetDeltaTime()`

---

## Architecture

```
/platform
    window.h/.cpp
    input.h/.cpp
    time.h/.cpp
```

---

## Deliverable

* Window opens
* Input events printed to console
* Mouse movement captured correctly

---

## Review Checklist

* [ ] No input lag visible
* [ ] Mouse delta stable
* [ ] No blocking calls in main thread
* [ ] Clean shutdown without crash

---

## Test Plan

* Move mouse rapidly → verify consistent deltas
* Hold multiple keys → verify no missed inputs
* Alt-tab → recover correctly

---

# Subphase 0.2 — Core Loop

## Goal

Deterministic simulation independent of rendering.

---

## Tasks

### 0.2.1 Fixed Timestep

Implement:

```
const float dt = 1.0f / 60.0f;
accumulator += frameTime;

while (accumulator >= dt) {
    Update(dt);
    accumulator -= dt;
}
```

### 0.2.2 Separation of Concerns

* `Update(dt)` → gameplay simulation
* `Render(alpha)` → interpolation later

### 0.2.3 Frame Limiting

* VSync ON initially
* Optional sleep for CPU stability

---

## Deliverable

* Object moving at constant speed regardless of FPS

---

## Review Checklist

* [ ] Movement identical at 30 FPS vs 120 FPS
* [ ] No spiral of death (frame lag accumulation)
* [ ] Stable frame pacing

---

## Test Plan

* Artificially cap FPS to:

  * 30
  * 60
  * 120
    → verify identical simulation result

---

# Subphase 0.3 — Memory System

## Goal

Eliminate runtime allocations and fragmentation.

---

## Tasks

### 0.3.1 Linear Allocator

* Preallocate large block (e.g., 64–256 MB)
* Pointer bump allocation
* Reset per level/load (not per frame)

### 0.3.2 Frame Allocator

* Small fast allocator reset every frame
* Used for:

  * temporary buffers
  * debug data

### 0.3.3 Allocation Tracking

* Count allocations per frame
* Assert if > 0 during gameplay

---

## Architecture

```
/core
    memory.h/.cpp
    allocator_linear.h
    allocator_frame.h
```

---

## Deliverable

* Allocation stats printed each frame

---

## Review Checklist

* [ ] 0 allocations in Update/Render
* [ ] No memory leaks on shutdown
* [ ] Predictable memory usage

---

## Test Plan

* Stress test with repeated level reloads
* Track memory before/after

---

# Subphase 0.4 — Logging & Debug Systems

## Goal

Provide visibility into engine state without impacting performance.

---

## Tasks

### 0.4.1 Logging System

* Levels:

  * INFO
  * WARNING
  * ERROR

* Output:

  * Console
  * Optional file

### 0.4.2 Debug Overlay (minimal)

* FPS counter
* Frame time (ms)
* Memory usage

---

## Deliverable

* On-screen debug info

---

## Review Checklist

* [ ] Logging does not stall frame
* [ ] Overlay updates in real-time
* [ ] Toggle on/off at runtime

---

## Test Plan

* Spam logs → ensure no frame drop
* Toggle overlay repeatedly

---

# Subphase 0.5 — Job System (Optional but Recommended)

## Goal

Prepare for multi-core CPUs (Core 2 Quad / Switch)

---

## Tasks

### 0.5.1 Thread Pool

* Fixed number of worker threads
* Job queue

### 0.5.2 Simple API

```
SubmitJob(function)
WaitAll()
```

---

## Deliverable

* Parallel execution test (e.g., dummy workload)

---

## Review Checklist

* [ ] No race conditions
* [ ] Deterministic results
* [ ] Scales across 2–4 cores

---

## Test Plan

* Run 1000 small jobs
* Verify completion time scales with cores

---

# Integration Milestone — Phase 0 Complete

## Required Demo

A running application that:

* Opens a window
* Handles input reliably
* Runs fixed timestep simulation
* Displays debug overlay
* Uses zero allocations per frame

---

## Metrics to Record

* Idle frame time
* CPU usage
* Input latency
* Memory usage

---

# Failure Conditions (Do Not Proceed If)

* Frame pacing unstable
* Input inconsistent
* Memory allocations still happening
* Debugging visibility insufficient

---

# Notes for Future Phases

* This phase defines ALL constraints for performance
* Any instability here will multiply later
* Do not add rendering/gameplay until this is solid

---
