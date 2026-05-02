# Phase 3 — Core Gameplay (Combat Loop)

**Detailed, Reviewable, and Iterable Plan**

## Objective

Implement a **complete, responsive combat loop** with:

* True 3D aiming (Z-axis, pitch + yaw)
* Hitscan and projectile weapons
* Basic enemy AI
* Immediate, consistent feedback

This phase defines the **“Hellgate-like feel”**. Accuracy, latency, and consistency matter more than visual complexity.

---

# Success Criteria (Phase Gate)

Do NOT proceed unless:

* Crosshair aligns with actual hit point at all angles
* Shooting feels instantaneous (no perceptible lag)
* 20+ enemies active without CPU spikes
* Combat loop (move → aim → shoot → kill) is fully playable
* Frame time remains within 16.6 ms budget

---

# Subphase 3.1 — Aiming System (Z-Axis Critical)

## Goal

Accurate, camera-based aiming in full 3D.

---

## Tasks

### 3.1.1 Camera Ray Generation

* From camera position
* Direction derived from:

  * yaw
  * pitch

### 3.1.2 Crosshair Alignment

* Raycast from camera center
* Crosshair represents ray direction

### 3.1.3 Hit Resolution Priority

* Closest hit wins:

  1. Enemies
  2. World
  3. Nothing (max range)

---

## API

```cpp
Ray GetCameraRay();
HitResult Raycast(ray);
```

---

## Deliverable

* Crosshair always matches actual hit location

---

## Review Checklist

* [ ] No offset between crosshair and hit
* [ ] Works at extreme pitch angles
* [ ] No jitter when moving camera

---

## Test Plan

* Aim at:

  * close targets
  * far targets
  * vertical targets (above/below)

---

# Subphase 3.2 — Hitscan Weapon System

## Goal

Instant, deterministic shooting.

---

## Tasks

### 3.2.1 Weapon Definition

* Fire rate
* Damage
* Range

### 3.2.2 Fire Logic

* On input:

  * generate ray
  * perform raycast
  * apply damage

### 3.2.3 Fire Cooldown

* Enforce fire rate via timer

---

## Deliverable

* Functional hitscan weapon

---

## Review Checklist

* [ ] Shots hit exactly where aimed
* [ ] Fire rate consistent
* [ ] No frame-dependent behavior

---

## Test Plan

* Spam fire input
* Test at different FPS caps

---

# Subphase 3.3 — Projectile System

## Goal

Support slower, visible projectiles.

---

## Tasks

### 3.3.1 Projectile Data

* Position
* Velocity
* Lifetime

### 3.3.2 Movement

* Integrate per fixed timestep

### 3.3.3 Collision

* Raycast or swept AABB

---

## Deliverable

* Projectiles travel and hit targets

---

## Review Checklist

* [ ] No tunneling
* [ ] Stable trajectory
* [ ] Performance stable with many projectiles

---

## Test Plan

* Fire multiple projectiles simultaneously
* Test fast vs slow speeds

---

# Subphase 3.4 — Enemy System (Entity + FSM)

## Goal

Basic but scalable AI behavior.

---

## Tasks

### 3.4.1 Enemy Data Structure

* Position
* Health
* State

### 3.4.2 Finite State Machine

States:

* Idle
* Chase
* Attack
* Dead

### 3.4.3 Transitions

* Idle → Chase (player detected)
* Chase → Attack (in range)
* Attack → Chase (out of range)
* Any → Dead (health ≤ 0)

---

## Deliverable

* Enemies react to player

---

## Review Checklist

* [ ] No erratic state switching
* [ ] Predictable behavior
* [ ] Low CPU cost per enemy

---

## Test Plan

* Spawn 20–50 enemies
* Observe transitions under load

---

# Subphase 3.5 — Damage System

## Goal

Consistent and extensible damage handling.

---

## Tasks

### 3.5.1 Damage Model

* Flat damage (start simple)

### 3.5.2 Hit Application

* Reduce health
* Trigger state changes

### 3.5.3 Death Handling

* Disable collision
* Remove or mark entity

---

## Deliverable

* Enemies take damage and die

---

## Review Checklist

* [ ] No double hits per shot
* [ ] Health updates deterministic
* [ ] Dead entities handled cleanly

---

## Test Plan

* Rapid fire → verify correct damage accumulation
* Kill multiple enemies quickly

---

# Subphase 3.6 — Feedback Systems (Critical for Feel)

## Goal

Make combat feel responsive and satisfying.

---

## Tasks

### 3.6.1 Visual Feedback

* Hit markers
* Enemy flash on hit

### 3.6.2 Camera Feedback

* Slight recoil or shake

### 3.6.3 Timing

* Feedback must occur same frame as hit

---

## Deliverable

* Immediate visual confirmation of hits

---

## Review Checklist

* [ ] Feedback < 50 ms perceived delay
* [ ] Clearly visible at all distances
* [ ] No dependency on frame rate

---

## Test Plan

* Fire single shots → verify instant feedback
* Test under low FPS conditions

---

# Subphase 3.7 — Weapon System Abstraction

## Goal

Allow multiple weapon types without rewriting logic.

---

## Tasks

### 3.7.1 Weapon Interface

```cpp
struct Weapon {
    Fire();
    Update();
};
```

### 3.7.2 Types

* Hitscan weapon
* Projectile weapon

### 3.7.3 Data-Driven Setup (basic)

* Configurable parameters

---

## Deliverable

* Multiple weapon types working

---

## Review Checklist

* [ ] No duplicated logic
* [ ] Easy to add new weapon types
* [ ] No runtime allocations

---

## Test Plan

* Switch weapons during gameplay
* Stress test fire rates

---

# Subphase 3.8 — Targeting & Spatial Integration

## Goal

Integrate combat with world queries.

---

## Tasks

### 3.8.1 Spatial Queries

* Use Phase 2 system for:

  * nearby enemies
  * line-of-sight checks

### 3.8.2 Optimization

* Avoid checking all entities each frame

---

## Deliverable

* Efficient targeting system

---

## Review Checklist

* [ ] Query cost scales with nearby entities
* [ ] No full-world scans
* [ ] Stable CPU usage

---

## Test Plan

* Large enemy count
* Frequent targeting updates

---

# Integration Milestone — Phase 3 Complete

## Required Demo

A playable combat sandbox where:

* Player moves freely in 3D
* Can aim up/down accurately
* Can shoot enemies (hitscan + projectile)
* Enemies react and attack
* Combat loop is continuous and responsive

---

## Metrics to Record

* Input-to-hit latency
* CPU time (AI + combat)
* Raycast cost
* Projectile count vs performance

---

# Failure Conditions (Do Not Proceed If)

* Crosshair misalignment exists
* Combat feels delayed or inconsistent
* AI causes CPU spikes
* Frame time exceeds budget under load

---

# Revision Notes (Important Adjustments)

Compared to earlier plan:

* Elevated **aiming accuracy to primary constraint**
* Formalized **weapon abstraction early**
* Added **strict feedback timing requirements**
* Integrated **spatial queries into combat**
* Enforced **deterministic behavior across FPS**

These changes prevent rework in later phases (especially multiplayer or advanced AI if added).

---

# Notes for Next Phase

Phase 4 will:

* Optimize CPU/GPU bottlenecks
* Introduce Switch-specific constraints
* Enforce strict frame budgets

Before proceeding:

* Lock gameplay feel
* Avoid adding new mechanics

---
