# Phase 5 — Content Pipeline & Tooling

**Detailed, Reviewable, and Iterable Plan**

## Objective

Enable fast, repeatable creation of game content with:

* Deterministic asset pipeline
* Minimal runtime overhead
* Rapid iteration (edit → export → run)

This phase converts the engine from a prototype into a **production-capable system**.

---

# Success Criteria (Phase Gate)

Do NOT proceed unless:

* Assets load in < 1 second for typical levels
* Level iteration cycle < 60 seconds (edit → test)
* No runtime parsing of heavy formats (all preprocessed)
* Asset memory usage predictable and bounded
* Designers (you) can create content without touching engine code

---

# Subphase 5.1 — Asset Pipeline Architecture

## Goal

Separate **offline processing** from runtime.

---

## Tasks

### 5.1.1 Define Asset Types

* Meshes
* Textures
* Materials
* Levels

### 5.1.2 Offline vs Runtime

| Stage   | Responsibility           |
| ------- | ------------------------ |
| Offline | Convert, compress, pack  |
| Runtime | Load preprocessed binary |

### 5.1.3 Directory Structure

```id="q8kz7r"
/assets_raw
    /models
    /textures
    /levels

/assets_build
    /meshes
    /textures
    /levels
```

---

## Deliverable

* Clear separation between raw and built assets

---

## Review Checklist

* [ ] No raw formats used at runtime (e.g., OBJ, PNG optional)
* [ ] All runtime assets are binary
* [ ] Build step reproducible

---

## Test Plan

* Delete build folder → rebuild → verify identical output

---

# Subphase 5.2 — Mesh Pipeline

## Goal

Convert models into efficient runtime format.

---

## Tasks

### 5.2.1 Input Format

* Use Blender export (OBJ or FBX initially)

### 5.2.2 Conversion Tool

* Write converter:

  * load model
  * optimize
  * save binary

### 5.2.3 Optimization Steps

* Remove duplicate vertices
* Generate index buffer
* Quantize data if needed

### 5.2.4 Output Format

Binary structure:

* vertex buffer
* index buffer
* bounds (AABB)

---

## Deliverable

* Engine loads custom mesh format

---

## Review Checklist

* [ ] No runtime mesh processing
* [ ] Mesh loads instantly
* [ ] Memory usage minimized

---

## Test Plan

* Load large mesh repeatedly
* Measure load time

---

# Subphase 5.3 — Texture Pipeline

## Goal

Efficient texture usage and batching.

---

## Tasks

### 5.3.1 Texture Conversion

* Convert PNG → GPU-friendly format

### 5.3.2 Atlas Builder

* Combine textures into atlas
* Generate UV remaps

### 5.3.3 Mipmaps

* Pre-generate mip levels

---

## Deliverable

* Textures loaded from atlas

---

## Review Checklist

* [ ] Reduced texture binds
* [ ] Memory usage controlled
* [ ] Visual quality acceptable

---

## Test Plan

* Compare:

  * individual textures vs atlas
* Measure draw calls

---

# Subphase 5.4 — Material System

## Goal

Simple, low-cost material definition.

---

## Tasks

### 5.4.1 Material Data

* Texture reference
* Color tint (optional)

### 5.4.2 Material Batching

* Group by material ID

---

## Deliverable

* Materials applied to meshes

---

## Review Checklist

* [ ] Minimal shader variants
* [ ] No runtime material creation
* [ ] Works with batching system

---

## Test Plan

* Render scene with multiple materials
* Verify batching still effective

---

# Subphase 5.5 — Level Pipeline

## Goal

Create levels efficiently using external tools.

---

## Tasks

### 5.5.1 Authoring Tool

* Use Blender (recommended)

### 5.5.2 Export Format

* Export:

  * geometry
  * transforms
  * references

### 5.5.3 Conversion Step

* Convert to:

  * chunked grid
  * mesh references

---

## Deliverable

* Level exported and loaded in engine

---

## Review Checklist

* [ ] Export process < 30 seconds
* [ ] No manual editing required
* [ ] Level matches editor layout

---

## Test Plan

* Modify level → export → load → verify changes

---

# Subphase 5.6 — Data-Driven Gameplay

## Goal

Allow gameplay tuning without recompilation.

---

## Tasks

### 5.6.1 Weapon Configs

* Fire rate
* Damage
* Range

### 5.6.2 Enemy Configs

* Health
* Speed
* Behavior parameters

### 5.6.3 Format

* JSON initially → optional binary later

---

## Deliverable

* Gameplay parameters editable externally

---

## Review Checklist

* [ ] No code changes required for tuning
* [ ] Config load time negligible
* [ ] Data validated on load

---

## Test Plan

* Change weapon stats → reload → verify effect

---

# Subphase 5.7 — Asset Build System

## Goal

Automate pipeline.

---

## Tasks

### 5.7.1 Build Tool

* Script (Python or C++)
* Processes all assets

### 5.7.2 Dependency Tracking

* Only rebuild changed assets

### 5.7.3 Command

```bash id="3m7wfk"
build_assets --all
```

---

## Deliverable

* One-command asset build

---

## Review Checklist

* [ ] Fast incremental builds
* [ ] Deterministic output
* [ ] Errors clearly reported

---

## Test Plan

* Modify one asset → rebuild → verify only it updates

---

# Subphase 5.8 — Iteration Workflow

## Goal

Minimize iteration time.

---

## Tasks

### 5.8.1 Hot Reload (Optional)

* Reload:

  * textures
  * configs

### 5.8.2 Fast Restart

* Engine restart < 2 seconds

---

## Deliverable

* Rapid edit-test loop

---

## Review Checklist

* [ ] Iteration cycle < 60 seconds
* [ ] No crashes during reload
* [ ] Stable workflow

---

## Test Plan

* Repeat:

  * edit → build → run (10×)
* Measure total time

---

# Integration Milestone — Phase 5 Complete

## Required Demo

A workflow where:

* You create a level in Blender
* Export it
* Run asset build
* Launch engine
* Play updated content immediately

---

## Metrics to Record

* Asset build time
* Level load time
* Iteration cycle duration
* Memory usage per asset type

---

# Failure Conditions (Do Not Proceed If)

* Asset pipeline slow or manual
* Runtime depends on raw formats
* Iteration cycle too long (> 2 minutes)
* Asset memory usage uncontrolled

---

# Revision Notes (Important Adjustments)

Compared to earlier plan:

* Enforced **offline-first pipeline**
* Added **atlas generation early**
* Formalized **binary formats**
* Introduced **automated build system**
* Emphasized **iteration speed as primary KPI**

These changes prevent production bottlenecks later.

---

# Notes for Next Phase

Phase 6 will focus on:

* Animation
* Audio
* UI
* Game progression systems

Important:

* Keep pipeline simple
* Avoid overengineering tools
* Optimize for speed, not flexibility

---
