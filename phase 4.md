# Phase 4 — Performance Optimization & Switch Targeting

**Detailed, Reviewable, and Iterable Plan**

## Objective

Achieve **stable 60 FPS under worst-case scenarios** on:

* Low-end PC (Core 2 Quad class)
* Nintendo Switch 1 (handheld + docked constraints)

This phase enforces **hard performance budgets** and removes bottlenecks identified in earlier phases.

---

# Success Criteria (Phase Gate)

Do NOT proceed unless:

* Frame time ≤ 16.6 ms under stress
* CPU ≤ 8 ms, GPU ≤ 8 ms
* No frame spikes > 2 ms
* Performance stable with:

  * 20–50 enemies
  * large visible scene
* Switch build (or simulation constraints) runs at 60 FPS

---

# Subphase 4.1 — Profiling Infrastructure

## Goal

Gain full visibility into CPU and GPU costs.

---

## Tasks

### 4.1.1 CPU Profiling

Implement scoped timers:

```cpp
PROFILE_SCOPE("Update");
PROFILE_SCOPE("Render");
```

Track:

* per-system time
* min / max / average

### 4.1.2 GPU Profiling

* OpenGL timer queries
* Measure:

  * draw time
  * total GPU frame

### 4.1.3 On-Screen Profiler

Display:

* CPU frame breakdown
* GPU time
* draw calls

---

## Deliverable

* Real-time profiling overlay

---

## Review Checklist

* [ ] Profiling overhead < 0.2 ms
* [ ] Accurate timings (no spikes due to measurement)
* [ ] Toggleable at runtime

---

## Test Plan

* Compare profiling ON vs OFF
* Validate timing consistency

---

# Subphase 4.2 — Bottleneck Identification

## Goal

Find the top performance limits before optimizing blindly.

---

## Tasks

### 4.2.1 Stress Scenarios

Create repeatable tests:

* High enemy count
* Large scene visibility
* High projectile count

### 4.2.2 Measure

Identify:

* CPU-heavy systems
* GPU-heavy passes

---

## Deliverable

* Ranked list of bottlenecks

---

## Review Checklist

* [ ] Top 3 CPU bottlenecks identified
* [ ] Top 3 GPU bottlenecks identified
* [ ] Reproducible test scenes exist

---

## Test Plan

* Run each scenario 10×
* Verify consistent results

---

# Subphase 4.3 — CPU Optimization

## Goal

Reduce CPU time to ≤ 8 ms.

---

## Tasks

### 4.3.1 Hot Path Optimization

* Remove virtual function calls
* Inline critical code

### 4.3.2 Data-Oriented Layout

* Convert AoS → SoA where beneficial
* Improve cache locality

### 4.3.3 Reduce Branching

* Simplify logic in:

  * AI
  * collision
  * update loops

### 4.3.4 Job System Integration

* Parallelize:

  * AI updates
  * projectile updates
  * culling

---

## Deliverable

* Reduced CPU frame time

---

## Review Checklist

* [ ] CPU ≤ 8 ms under stress
* [ ] No race conditions introduced
* [ ] Deterministic results preserved

---

## Test Plan

* Compare before/after CPU timings
* Validate gameplay unchanged

---

# Subphase 4.4 — GPU Optimization

## Goal

Reduce GPU time to ≤ 8 ms.

---

## Tasks

### 4.4.1 Draw Call Reduction

* Aggressive batching
* Merge materials

### 4.4.2 State Change Minimization

* Sort render queue:

  1. shader
  2. texture
  3. mesh

### 4.4.3 Geometry Reduction

* Lower poly counts if needed
* Add simple LOD system

### 4.4.4 Shader Simplification

* Remove unnecessary calculations
* Avoid dynamic branching in shaders

---

## Deliverable

* Stable GPU frame time

---

## Review Checklist

* [ ] Draw calls ≤ 300–500
* [ ] GPU ≤ 8 ms
* [ ] No visual regressions breaking gameplay clarity

---

## Test Plan

* Measure GPU time across scenes
* Compare LOD vs non-LOD

---

# Subphase 4.5 — Memory Optimization

## Goal

Fit within Switch memory constraints and improve cache performance.

---

## Tasks

### 4.5.1 Memory Budgeting

Define limits:

* textures
* meshes
* world data

### 4.5.2 Reduce Footprint

* Compress textures where possible
* Use shared meshes

### 4.5.3 Cache Efficiency

* Align data structures
* Avoid pointer chasing

---

## Deliverable

* Predictable memory usage

---

## Review Checklist

* [ ] No memory spikes
* [ ] Fits within target limits
* [ ] Stable over long sessions

---

## Test Plan

* Long runtime test (30+ minutes)
* Monitor memory usage

---

# Subphase 4.6 — Switch-Specific Constraints (Simulation First)

## Goal

Prepare for Nintendo Switch hardware limitations.

---

## Tasks

### 4.6.1 Resolution Scaling

* Dynamic resolution (e.g., 720p → lower if needed)

### 4.6.2 Reduced Draw Distance

* Limit far objects
* Aggressive culling

### 4.6.3 CPU Budget Awareness

* Assume fewer effective cores than PC
* Reduce job count accordingly

### 4.6.4 GPU Budget Awareness

* Avoid heavy fragment shaders
* Prefer vertex/lightmap solutions

---

## Deliverable

* “Switch-mode” configuration on PC

---

## Review Checklist

* [ ] Stable 60 FPS in reduced mode
* [ ] Visual quality acceptable
* [ ] No gameplay impact

---

## Test Plan

* Force reduced settings
* Measure frame stability

---

# Subphase 4.7 — Frame Pacing & Stability

## Goal

Eliminate stutter and ensure consistent frame delivery.

---

## Tasks

### 4.7.1 Frame Time Smoothing

* Monitor frame variance

### 4.7.2 Avoid Spikes

* Remove:

  * hidden allocations
  * blocking calls
  * large per-frame work

### 4.7.3 VSync Validation

* Ensure proper sync behavior

---

## Deliverable

* Smooth, consistent frame pacing

---

## Review Checklist

* [ ] No visible stutter
* [ ] Frame variance < 1 ms typical
* [ ] No periodic spikes

---

## Test Plan

* Long gameplay session
* Record frame time graph

---

# Integration Milestone — Phase 4 Complete

## Required Demo

A stress-tested build where:

* Large scene rendered
* 20–50 enemies active
* Multiple projectiles
* Stable 60 FPS maintained continuously

---

## Metrics to Record

* CPU frame time (avg / max)
* GPU frame time
* Draw calls
* Memory usage
* Frame variance

---

# Failure Conditions (Do Not Proceed If)

* Frame drops below 60 FPS
* CPU or GPU exceeds 8 ms consistently
* Frame spikes visible
* Performance depends heavily on scene complexity

---

# Revision Notes (Important Adjustments)

Compared to earlier plan:

* Introduced **strict profiling-first workflow**
* Added **Switch simulation mode before porting**
* Enforced **frame pacing constraints**
* Elevated **data-oriented optimization**
* Formalized **stress test scenarios**

These changes ensure performance is validated, not assumed.

---

# Notes for Next Phase

Phase 5 will focus on:

* Asset pipeline
* Level creation workflow
* Data-driven systems

Important:

* Do NOT add new gameplay features until performance is locked
* Any new feature must pass performance budget immediately

---
