# Custom Game Engine & Game Development Plan

**Target:** Nintendo Switch 1 + Low-End PC (Core 2 Quad)
**Performance Goal:** Stable 60 FPS
**Visual Style:** Barony-like (low-poly, stylized)
**Gameplay Feel:** Hellgate: London (real-time combat, vertical aiming, Z-axis freedom)

---

# GLOBAL TECHNICAL TARGETS

## Platforms

* PC: Core 2 Quad + low-end GPU (GTX 750 class baseline)
* Nintendo Switch 1 (Maxwell GPU, limited CPU)

## Performance Budget

* Frame time: **16.6 ms**
* CPU: ≤ 8 ms
* GPU: ≤ 8 ms

## Rendering Constraints

* OpenGL 3.3 (PC) → abstraction layer → Switch backend later
* Forward rendering only
* Draw calls: 300–500 max
* Visible triangles: 150k–300k

---

# PHASE 0 — TECH FOUNDATION

## Goal

Stable runtime loop and platform abstraction.

---

## Subphase 0.1 – Platform Layer

### Tasks

* Window creation (SDL2 recommended)
* Input abstraction (keyboard, mouse, controller)
* High precision timing system

### Deliverable

* Window opens and prints input events

### Review Criteria

* Input latency < 1 frame
* Stable delta time

---

## Subphase 0.2 – Core Loop

### Tasks

* Fixed timestep loop (60 Hz simulation)
* Separate update and render

### Deliverable

* Deterministic simulation test

### Review Criteria

* No simulation drift over time
* Stable frame pacing

---

## Subphase 0.3 – Memory + Logging

### Tasks

* Arena or linear allocator
* Debug logging system

### Deliverable

* Allocation tracking per frame

### Review Criteria

* Zero allocations during gameplay loop

---

# PHASE 1 — RENDERING CORE

## Goal

Efficient stylized rendering at 60 FPS.

---

## Subphase 1.1 – Minimal Renderer

### Tasks

* OpenGL initialization
* Shader system
* Mesh rendering (VBO/VAO)

### Deliverable

* Rotating cube

### Review Criteria

* Minimal GPU usage (<0.2 ms simple scene)

---

## Subphase 1.2 – Camera System

### Tasks

* FPS camera (yaw + pitch)
* Projection and view matrices

### Deliverable

* Free-look camera

### Review Criteria

* No jitter
* Correct perspective

---

## Subphase 1.3 – Batch Rendering

### Tasks

* Static batching
* Texture atlases

### Deliverable

* Scene rendered under 200 draw calls

### Review Criteria

* Measurable draw call cap

---

## Subphase 1.4 – Lighting Model

### Tasks

* Single directional light
* Vertex lighting or lightmaps

### Deliverable

* Stylized visual output

### Review Criteria

* No dynamic shadows
* Stable performance

---

## Subphase 1.5 – Visibility

### Tasks

* Frustum culling
* Optional grid/sector culling

### Deliverable

* Large scene rendering efficiently

### Review Criteria

* Performance independent of total world size

---

# PHASE 2 — WORLD SYSTEM

## Goal

3D world with grid-assisted gameplay logic.

---

## Subphase 2.1 – Spatial Structure

### Tasks

* 3D grid system
* Chunk-based world division

### Deliverable

* Dynamic chunk loading/unloading

### Review Criteria

* Constant memory per active area

---

## Subphase 2.2 – Collision System

### Tasks

* AABB collision
* Raycasting

### Deliverable

* Player interacts with world

### Review Criteria

* No tunneling at normal speeds

---

## Subphase 2.3 – Player Controller

### Tasks

* Movement (WASD)
* Jump + gravity
* Stair stepping

### Deliverable

* Smooth navigation

### Review Criteria

* No jitter
* Stable movement speed

---

## Subphase 2.4 – Level Format

### Tasks

* Define simple format (JSON or binary)
* Load meshes and collision

### Deliverable

* Loadable test level

### Review Criteria

* Load time < 1 second

---

# PHASE 3 — CORE GAMEPLAY

## Goal

Complete combat loop.

---

## Subphase 3.1 – Aiming System

### Tasks

* Raycast from camera center
* Pitch-based aiming

### Deliverable

* Accurate crosshair targeting

### Review Criteria

* No offset errors
* Works at all angles

---

## Subphase 3.2 – Shooting System

### Tasks

* Hitscan weapons
* Impact detection

### Deliverable

* Functional shooting

### Review Criteria

* Deterministic hit results

---

## Subphase 3.3 – Projectile System

### Tasks

* Projectile motion
* Collision detection

### Deliverable

* Visible projectiles

### Review Criteria

* Stable at 60 FPS

---

## Subphase 3.4 – Enemy AI

### Tasks

* Finite state machine:

  * Idle
  * Chase
  * Attack
  * Dead

### Deliverable

* Enemy engages player

### Review Criteria

* No CPU spikes with many enemies

---

## Subphase 3.5 – Damage + Feedback

### Tasks

* Health system
* Hit reactions

### Deliverable

* Responsive combat

### Review Criteria

* Immediate feedback (<50 ms perceived)

---

# PHASE 4 — PERFORMANCE & SWITCH TARGETING

## Goal

Lock 60 FPS on weakest hardware.

---

## Subphase 4.1 – Profiling

### Tasks

* CPU timers
* GPU timing queries

### Deliverable

* Frame breakdown

### Review Criteria

* Identify main bottlenecks

---

## Subphase 4.2 – CPU Optimization

### Tasks

* Remove virtual calls
* Convert hot paths to SoA

### Deliverable

* Reduced CPU time

---

## Subphase 4.3 – GPU Optimization

### Tasks

* Reduce state changes
* Merge materials

### Deliverable

* Stable GPU time

---

## Subphase 4.4 – Switch Constraints

### Tasks

* Dynamic resolution scaling
* Reduced draw distance

### Deliverable

* Switch prototype build

### Review Criteria

* Stable 60 FPS handheld

---

# PHASE 5 — CONTENT PIPELINE

## Goal

Efficient content creation workflow.

---

## Subphase 5.1 – Asset Pipeline

### Tasks

* Model conversion
* Texture atlas builder

### Deliverable

* Fast loading assets

---

## Subphase 5.2 – Level Creation Workflow

### Tasks

* External editor (e.g. Blender)
* Export pipeline

### Deliverable

* Rapid level creation

---

## Subphase 5.3 – Data-Driven Systems

### Tasks

* Weapon configs
* Enemy configs

### Deliverable

* Tunable gameplay without recompiling

---

# PHASE 6 — POLISH & EXTENSION

## Goal

Complete game feel.

---

## Subphase 6.1 – Animation

### Tasks

* Basic animation system
* Weapon animations

---

## Subphase 6.2 – Audio

### Tasks

* Spatial audio
* Sound effects

---

## Subphase 6.3 – UI

### Tasks

* HUD
* Crosshair

---

## Subphase 6.4 – Game Loop

### Tasks

* Progression system
* Loot system

---

# ITERATION STRATEGY

After each subphase:

1. Build a test scene
2. Measure performance (FPS, CPU, GPU)
3. Validate gameplay feel
4. Refactor if needed

---

# MINIMUM VIABLE MILESTONE

* Player moves in 3D
* Can aim vertically
* Can shoot enemies
* Stable 60 FPS

---

# REALISTIC TIMELINE

* Phase 0–2: 2–4 months
* Phase 3: 2–3 months
* Phase 4: 1–2 months
* Phase 5–6: ongoing

---

# CORE PRINCIPLE

Keep systems simple, performance-focused, and tightly scoped to the game.
