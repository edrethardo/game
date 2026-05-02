# Phase 1 — Rendering Core (Detailed, Iterable Plan)

## Objective

Build a minimal, high-performance renderer capable of:

* Stable 60 FPS on low-end PC and Switch-class hardware
* Barony-like visual style (low-poly, stylized)
* Strict control over draw calls and GPU cost

This renderer must remain **simple, predictable, and tightly scoped**.

---

# Success Criteria (Phase Gate)

You do NOT proceed unless:

* Scene renders at 60 FPS with < 4 ms GPU time (PC baseline)
* Draw calls ≤ 300 in stress scene
* No per-frame allocations
* Camera movement is smooth and jitter-free
* Rendering cost scales with visible objects, not total objects

---

# Subphase 1.1 — Graphics Initialization

## Goal

Create a stable OpenGL rendering context and baseline pipeline.

---

## Tasks

### 1.1.1 OpenGL Context

* Initialize OpenGL 3.3 core profile
* Enable:

  * Depth testing
  * Backface culling

### 1.1.2 State Setup

* Default states:

  * Depth func: LESS
  * Cull: BACK
  * Clear color

### 1.1.3 Debug Output (important)

* Enable OpenGL debug callback
* Log errors/warnings

---

## Architecture

```
/renderer
    renderer.h/.cpp
    gl_context.h/.cpp
```

---

## Deliverable

* Window clears with color
* No GL errors

---

## Review Checklist

* [ ] No GL warnings in debug output
* [ ] Context stable after resize
* [ ] No redundant state changes

---

## Test Plan

* Resize window repeatedly
* Toggle fullscreen

---

# Subphase 1.2 — Shader System

## Goal

Create minimal, reusable shader pipeline.

---

## Tasks

### 1.2.1 Shader Loader

* Load from file
* Compile + link
* Error reporting

### 1.2.2 Uniform Handling

* Set:

  * Matrices
  * Colors
  * Textures

### 1.2.3 Basic Shader

Vertex:

* Position
* MVP transform

Fragment:

* Flat color or texture

---

## Deliverable

* Render colored triangle

---

## Review Checklist

* [ ] Shader errors clearly logged
* [ ] No redundant uniform updates
* [ ] Shader switching cost measurable

---

## Test Plan

* Reload shader at runtime (optional)
* Force compile error → verify log

---

# Subphase 1.3 — Mesh System

## Goal

Efficient rendering of geometry.

---

## Tasks

### 1.3.1 Mesh Format

* Positions
* Normals (optional early)
* UVs

### 1.3.2 GPU Upload

* VBO + VAO
* Static draw usage

### 1.3.3 Draw Call API

```
DrawMesh(mesh, transform)
```

---

## Deliverable

* Render cube and multiple instances

---

## Review Checklist

* [ ] No per-frame buffer uploads
* [ ] Mesh reused across instances
* [ ] Stable memory usage

---

## Test Plan

* Render 1000 cubes
* Measure draw calls and FPS

---

# Subphase 1.4 — Camera System

## Goal

Implement FPS-style camera with vertical aiming support.

---

## Tasks

### 1.4.1 Camera Math

* View matrix (look direction)
* Projection matrix (FOV, aspect)

### 1.4.2 Controls

* Mouse:

  * Yaw (horizontal)
  * Pitch (vertical, clamped)

### 1.4.3 Movement (basic)

* Forward/back/strafe

---

## Deliverable

* Smooth free-look camera

---

## Review Checklist

* [ ] No jitter or drift
* [ ] Pitch clamped correctly
* [ ] Movement frame-rate independent

---

## Test Plan

* Rapid mouse movement
* Extreme pitch angles

---

# Subphase 1.5 — Texture System

## Goal

Efficient texture handling with batching in mind.

---

## Tasks

### 1.5.1 Texture Loading

* Use stb_image
* Upload to GPU

### 1.5.2 Texture Binding Strategy

* Minimize state changes
* Group by texture

### 1.5.3 Texture Atlas Support

* Combine multiple textures into one

---

## Deliverable

* Textured cubes

---

## Review Checklist

* [ ] No redundant texture binds
* [ ] Atlas reduces draw calls
* [ ] Memory usage controlled

---

## Test Plan

* Render scene with 10+ textures
* Compare draw calls with/without atlas

---

# Subphase 1.6 — Batch Rendering

## Goal

Reduce draw calls aggressively.

---

## Tasks

### 1.6.1 Static Batching

* Merge meshes sharing material

### 1.6.2 Instance Rendering (optional)

* Same mesh, multiple transforms

### 1.6.3 Render Queue

* Sort by:

  1. Shader
  2. Texture
  3. Mesh

---

## Deliverable

* Scene rendered under 200–300 draw calls

---

## Review Checklist

* [ ] Draw calls measurable and capped
* [ ] Sorting reduces state changes
* [ ] No CPU spikes during batching

---

## Test Plan

* Large scene (500+ objects)
* Measure draw calls before/after batching

---

# Subphase 1.7 — Lighting Model (Barony Style)

## Goal

Cheap, stylized lighting.

---

## Tasks

### 1.7.1 Directional Light

* Single light source
* Simple diffuse shading

### 1.7.2 Vertex Lighting OR Lightmaps

* Prefer baked lighting for performance

### 1.7.3 Ambient Term

* Flat ambient to avoid black shadows

---

## Deliverable

* Stylized lit scene

---

## Review Checklist

* [ ] No dynamic shadows
* [ ] Lighting cost minimal
* [ ] Consistent look across scene

---

## Test Plan

* Compare lit vs unlit cost
* Validate visual clarity

---

# Subphase 1.8 — Visibility System

## Goal

Render only what is visible.

---

## Tasks

### 1.8.1 Frustum Culling

* Per-object bounding box test

### 1.8.2 Spatial Partition (simple grid)

* Divide world into sectors

### 1.8.3 Debug Visualization

* Show culled vs visible objects

---

## Deliverable

* Large scene with stable FPS

---

## Review Checklist

* [ ] FPS independent of total objects
* [ ] Culling cost < 1 ms CPU
* [ ] No popping artifacts

---

## Test Plan

* 10,000 objects in scene
* Move camera → verify stable FPS

---

# Integration Milestone — Phase 1 Complete

## Required Demo

A scene that includes:

* 3D environment (hundreds of objects)
* Textures + lighting
* Free-look camera
* Culling active

---

## Metrics to Record

* GPU frame time
* CPU render time
* Draw calls
* Visible vs total objects

---

# Failure Conditions (Do Not Proceed If)

* Draw calls too high (>500)
* GPU frame time unstable
* Camera jitter present
* Rendering tied to total scene size

---

# Revision Notes (Important Adjustment)

Compared to the previous high-level plan:

* Added **explicit batching + render queue sorting**
* Added **strict draw call targets**
* Emphasized **texture atlas early**
* Deferred advanced features (shadows, PBR, post-processing) entirely

This keeps the renderer aligned with Switch + low-end PC constraints.

---

# Notes for Next Phase

* Rendering must remain stable before adding world complexity
* Do NOT add physics or gameplay into renderer
* Phase 2 will reuse:

  * Mesh system
  * Camera
  * Culling

---
