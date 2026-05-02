# Phase 2 — World System (Detailed, Iterable Plan)

## Objective

Build a **3D world representation with grid-assisted gameplay logic** that enables:

* Fast collision and raycasting
* Efficient streaming (chunk-based)
* Deterministic behavior for gameplay systems
* Scalability to large levels without performance loss

This phase bridges rendering and gameplay.

---

# Success Criteria (Phase Gate)

You do NOT proceed unless:

* Player can move through a 3D world without jitter
* Collision is stable (no tunneling, no sticking)
* Raycasts are accurate and fast (<0.1 ms typical)
* World size does not impact FPS significantly
* Memory usage remains bounded via chunking

---

# Subphase 2.1 — World Representation (Hybrid Grid + Mesh)

## Goal

Combine:

* Grid for logic (AI, collision, gameplay)
* Meshes for rendering

---

## Tasks

### 2.1.1 Grid Definition

* 3D grid (uniform cells)
* Cell size: ~1m (tune later)

Each cell stores:

* Solid / empty
* Optional metadata (material, type)

### 2.1.2 Chunking System

* Divide world into chunks (e.g., 16×16×16 cells)
* Each chunk:

  * Logical grid data
  * Reference to render mesh

### 2.1.3 World Manager

* Load/unload chunks
* Maintain active chunk list

---

## Architecture

```id="1cxfjs"
/world
    world.h/.cpp
    chunk.h/.cpp
    grid.h/.cpp
```

---

## Deliverable

* Load a multi-chunk test world
* Render meshes aligned with grid

---

## Review Checklist

* [ ] Chunk boundaries invisible to player
* [ ] Memory usage proportional to active chunks
* [ ] Grid and mesh aligned correctly

---

## Test Plan

* Move across chunk boundaries
* Load/unload chunks dynamically

---

# Subphase 2.2 — Collision System (AABB-Based)

## Goal

Simple, robust collision without full physics engine.

---

## Tasks

### 2.2.1 Player Collider

* Capsule or AABB (start with AABB for simplicity)

### 2.2.2 Static World Collision

* Grid-based collision lookup
* Check occupied cells

### 2.2.3 Collision Resolution

* Axis-separated resolution:

  * X → resolve
  * Y → resolve
  * Z → resolve

---

## Deliverable

* Player collides with world geometry

---

## Review Checklist

* [ ] No penetration through walls
* [ ] No jitter when sliding along surfaces
* [ ] Stable at high movement speeds

---

## Test Plan

* Move diagonally into walls
* Slide along edges
* Test corner cases (literally)

---

# Subphase 2.3 — Raycasting System (Critical for Gameplay)

## Goal

Accurate and fast raycasts for:

* Shooting
* Interaction
* AI visibility

---

## Tasks

### 2.3.1 Grid Raycast (DDA algorithm)

* Step through grid cells efficiently

### 2.3.2 Mesh Raycast (optional later)

* For precise hit detection

### 2.3.3 API

```id="q2y8tp"
Raycast(origin, direction, maxDistance)
```

Returns:

* Hit position
* Hit normal
* Hit entity/cell

---

## Deliverable

* Debug raycast visualization

---

## Review Checklist

* [ ] No missed hits
* [ ] Stable at all angles
* [ ] Fast (<0.1 ms typical)

---

## Test Plan

* Shoot rays at:

  * walls
  * corners
  * long distances

---

# Subphase 2.4 — Player Controller

## Goal

Responsive movement with “game feel” foundation.

---

## Tasks

### 2.4.1 Movement System

* WASD relative to camera
* Acceleration + deceleration

### 2.4.2 Gravity + Jump

* Constant downward force
* Jump impulse

### 2.4.3 Ground Detection

* Raycast or collision-based

### 2.4.4 Stair Stepping (important)

* Allow stepping over small obstacles

---

## Deliverable

* Smooth player navigation in 3D space

---

## Review Checklist

* [ ] No jitter on slopes or steps
* [ ] Movement consistent across FPS
* [ ] Jump feels responsive

---

## Test Plan

* Walk up stairs
* Jump repeatedly
* Move across uneven terrain

---

# Subphase 2.5 — Level Format & Loading

## Goal

Define a simple, fast-loading level format.

---

## Tasks

### 2.5.1 Format Design

Choose:

* Binary (preferred for speed)
* JSON (for debugging early)

Content:

* Chunk data
* Mesh references
* Collision grid

### 2.5.2 Loader

* Load world into memory
* Initialize chunks

### 2.5.3 Hot Reload (optional)

* Reload level without restart

---

## Deliverable

* Load test level in <1 second

---

## Review Checklist

* [ ] No blocking stutter during load
* [ ] Data integrity verified
* [ ] Easy to extend format

---

## Test Plan

* Load large level
* Reload repeatedly

---

# Subphase 2.6 — Spatial Queries & Optimization

## Goal

Efficient lookup for gameplay systems.

---

## Tasks

### 2.6.1 Spatial Index

* Map entities to grid cells

### 2.6.2 Query API

```id="9l1l3o"
GetEntitiesInRadius(position, radius)
```

### 2.6.3 Cache-Friendly Layout

* Store grid in contiguous memory

---

## Deliverable

* Fast entity queries

---

## Review Checklist

* [ ] Query time scales with local density
* [ ] No full-world scans
* [ ] Cache-friendly memory layout

---

## Test Plan

* Spawn many entities
* Query repeatedly

---

# Subphase 2.7 — Debug Tools (World)

## Goal

Make world systems visible and debuggable.

---

## Tasks

### 2.7.1 Debug Rendering

* Draw:

  * Grid cells
  * Collision boxes
  * Raycasts

### 2.7.2 Toggle Modes

* Enable/disable overlays at runtime

---

## Deliverable

* Visual debugging tools

---

## Review Checklist

* [ ] No performance impact when disabled
* [ ] Clear visualization
* [ ] Easy toggling

---

## Test Plan

* Enable all debug views
* Verify correctness visually

---

# Integration Milestone — Phase 2 Complete

## Required Demo

A playable prototype where:

* Player moves in a 3D world
* Collision works reliably
* Raycasts hit correctly
* World streams via chunks
* Performance remains stable

---

## Metrics to Record

* Collision time (CPU)
* Raycast time
* Memory usage (per chunk)
* FPS across large world

---

# Failure Conditions (Do Not Proceed If)

* Collision jitter present
* Raycasts unreliable
* World size affects FPS heavily
* Memory usage unbounded

---

# Revision Notes (Important Adjustments)

Compared to earlier plan:

* Introduced **grid + chunk hybrid explicitly**
* Added **DDA raycasting (critical for shooting later)**
* Elevated **stair stepping** to required feature
* Added **spatial queries for future AI/combat**
* Emphasized **cache-friendly layouts**

These changes ensure Phase 3 (combat) will not require rewrites.

---

# Notes for Next Phase

* Phase 3 will heavily depend on:

  * Raycasting
  * Player controller
  * Spatial queries

* Any instability here will break combat feel

* Do NOT add:

  * Physics engine
  * Complex interactions
    yet

---
