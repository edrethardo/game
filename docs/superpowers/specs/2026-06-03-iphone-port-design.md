# iPhone Port — Design Spec

**Date:** 2026-06-03
**Status:** Draft (brainstorming output, pre-implementation-plan)
**Project:** DungeonEngine (custom C++17 dungeon-crawler; SDL2 + OpenGL 3.3; targets Switch + low-end PC)

## Goal

Get DungeonEngine running on the author's **own iPhone** for personal play/testing, built and
deployed **entirely from a Linux machine with no Mac and a free Apple ID** (no paid Apple Developer
Program membership). First objective is "**get it running**" in singleplayer with a physical
controller; debugging depth and multiplayer come later.

## Constraints (hard)

- **Build host: Linux only.** No macOS/Xcode available, now or planned. The toolchain must run on
  Linux end-to-end.
- **Free Apple ID** (personal team): development signing only, **7-day certificate expiry**, device
  must be registered, no App Store / ad-hoc / enterprise distribution, limited entitlements.
- **EU** location: relevant only to *distribution* (DMA alternative-distribution still needs a paid
  account); **no effect on the self-debug path**. Noted and set aside.
- **Controller-only input** (e.g. **GameSir G8**): the app requires a connected controller; there are
  **no touch controls**.
- **Deploy over USB** as the reliable path; first connection is USB-mandatory regardless of later
  options (pairing/trust, Developer Mode, cert trust).

## Non-goals (YAGNI — explicitly out of scope for this effort)

- **Touch controls / virtual on-screen UI** — controller-only; the menus/HUD already support gamepad
  navigation (from the Switch work).
- **App Store / TestFlight / EU alternative distribution** — personal device only.
- **Metal renderer** — OpenGL ES 3.0 instead (see Decisions).
- **Interactive on-device `lldb`** in the first cut — iterate with deploy + device logs + crash
  reports; interactive debugging is a later, optional milestone.
- **Multiplayer** in the first milestone — a clean later phase (Phase 4).
- **Wi-Fi / OTA install** as the primary loop — USB first; Wi-Fi is an optional later convenience.

## Key decisions

1. **Graphics: OpenGL ES 3.0** via SDL2's GLES context (not Metal, not ANGLE for now). GL 3.3 is
   approximately a superset of GL ES 3.0, so this is a *port* (shaders + a few API calls), not a
   rewrite. GLSL `#version 330` → `#version 300 es` + precision qualifiers; replace ES-absent calls
   (e.g. `glPolygonMode` wireframe debug → explicit line draws); verify texture/format and any
   `GL_ARB_*` usage. **Testable on Linux** with Mesa's GLES 3 driver, so the renderer port is
   developed/validated *without the device loop*. Deprecated on iOS but functional on all current
   iPhones for the foreseeable future. ANGLE (GL ES→Metal) is the future hedge if deprecation ever
   bites; Metal is a deliberate non-goal.
2. **Platform: reuse SDL2** (already the engine's window/input/audio/clock abstraction in
   `src/platform/`). SDL2 officially supports iOS: UIKit app lifecycle, `SDL_GameController` over the
   GameController framework, CoreAudio (SDL2_mixer), retina/high-DPI. This is the single biggest
   reuse lever — most of `src/platform/` carries over.
3. **Toolchain: `xtool`-first, manual cross-toolchain as fallback.** `xtool` (open-source, Linux)
   bundles a Darwin/iOS SDK, free-Apple-ID signing, and `libimobiledevice`-based install. Evaluate it
   first via the Phase-0 spike. If it can't drive a CMake/C++/SDL2 build, fall back to a manual
   pipeline: cross-`clang` + extracted iOS SDK + Mach-O linker, sign with `zsign`/AltSign-style flow,
   install with `libimobiledevice`. CMake drives compilation via an iOS toolchain file in either case.
4. **Deploy: USB** (`xtool install` / `ideviceinstaller`). First-time setup over USB is mandatory:
   pair/trust, enable **Developer Mode** (iOS 16+), trust the free cert on-device. Wi-Fi install (via
   `usbmuxd` network mode after pairing) is an *optional later* convenience, not depended upon.
5. **Input: controller-only.** Map the GameSir G8 through `SDL_GameController`; the app declares a
   controller is required. Reuse the existing gamepad bindings and gamepad menu/HUD navigation.

## Architecture / workstreams

Each is an independently-buildable unit with a clear interface to the rest of the engine.

- **Build & toolchain** (`platform/ios/` scripts + CMake iOS toolchain file): iOS SDK acquisition
  (via xtool or extraction), cross-compile SDL2 + SDL2_mixer + the engine, assemble the `.app`
  bundle, free-account sign, USB install, and a **weekly re-sign + reinstall** script. *Depends on:*
  Linux toolchain + a registered device. *Interface:* `make ios` / a `./tools/ios_deploy.sh`.
- **Renderer — GL ES 3.0 backend** (`src/renderer/`): GLES context creation through SDL2; shader port
  (GLSL 330 → 300 es + precision); replace ES-absent API; format/extension audit (instanced draws
  used by the projectile renderer are core in ES 3.0 ✓). *Validated on Linux (Mesa GLES) first.*
  *Interface:* unchanged `Renderer::*` API to the rest of the engine.
- **Platform layer — SDL2 iOS** (`src/platform/`): app lifecycle (background/resume, audio
  interruption, `SDL_APP_*` events), controller hot-plug, orientation/retina/safe-area, wall-clock.
  *Interface:* the existing `Window`/`Input`/`Clock` abstractions.
- **Filesystem / IO**: assets ship **read-only inside the `.app` bundle** (`SDL_GetBasePath`); saves
  go to the **sandboxed `Documents` dir** (`SDL_GetPrefPath`). Centralize the two base paths so
  asset-load and savegame code stop assuming cwd-relative paths. *Interface:* a small path-resolver
  used by asset loaders + `engine_persist`.
- **App packaging**: `Info.plist` (`GCSupportsControllerUserInteraction`, controller-required,
  supported orientations, bundle id, min-iOS, and `NSLocalNetworkUsageDescription` reserved for
  Phase 4 MP), app icon, launch screen, asset catalog. *Interface:* consumed by the bundle-assembly
  step of the toolchain.

## Phases / milestones (de-risk-first)

This spec is the umbrella; **each phase becomes its own implementation plan**, executed one at a
time. **Phase 0 is planned and built first** — its outcome (xtool vs. manual toolchain, and whether
the Linux→iOS pipeline is viable at all) determines the shape and feasibility of every later phase,
so there's no value in fully planning Phases 1–4 until Phase 0 lands.

- **Phase 0 — Pipeline spike (riskiest thing first).** A trivial SDL2 + GL ES 3.0 "hello triangle":
  built on Linux, signed with the free Apple ID, installed and launched on the iPhone via
  xtool + `libimobiledevice`, reading `idevicesyslog`, and quit with the G8 controller.
  **Exit criteria:** a triangle on the device, controller input confirmed, and a *repeatable*
  build→sign→install script. This validates SDK → cross-compile → sign → install → run → controller →
  logs before the real engine is touched, and decides xtool-vs-manual.
- **Phase 1 — GL ES 3.0 renderer.** Port the engine renderer to GL ES 3.0, validated on Linux (Mesa
  GLES 3). **Exit:** the game renders correctly through the GLES path on the Linux dev box.
- **Phase 2 — iOS app integration.** Lifecycle, asset/save paths, `Info.plist`, controller mapping,
  audio. **Exit:** the engine boots to its menu on the device and is navigable with the G8.
- **Phase 3 — Singleplayer playable on device.** Full SP loop on the phone; scripted **7-day re-sign
  + redeploy**; deploy-and-log iteration. **Exit:** a complete singleplayer dungeon run on the iPhone
  with working audio and saves that persist across launches.
- **Phase 4 — (later) Multiplayer.** ENet over LAN/internet, the iOS Local-Network permission
  (`NSLocalNetworkUsageDescription`), replace/drop UPnP (`miniupnpc` viability on iOS is doubtful;
  likely LAN-only or manual-IP first), and a perf pass.

## Risks & mitigations

- **iOS SDK acquisition is unofficial / fragile** (Apple's SDK license is macOS-only; extraction from
  Xcode `.xip` is a gray area). → Prefer xtool's bundled SDK; document whichever path is used; accept
  that the whole pipeline is unofficial.
- **xtool ↔ CMake/C++ fit is unproven.** → Phase 0 spike either validates it or triggers the manual
  cross-toolchain fallback before any engine work is spent.
- **GL ES deprecation on iOS.** → Functional for years; ANGLE is the future hedge; not a near-term
  concern.
- **7-day cert treadmill.** → A scripted re-sign+reinstall; keep the device paired/trusted; weekly
  cabled refresh.
- **Performance/thermals.** → A modern iPhone substantially exceeds the Switch (Tegra X1) target the
  engine already runs on; expected to be fine. Profile in Phase 3.
- **On-device debugging is limited from Linux.** → Logs (`idevicesyslog`) + crash reports
  (`idevicecrashreport`) first; interactive `lldb` (debugserver + `iproxy`) is an optional later add.

## Verification / success criteria

- **Phase 0:** a triangle on the iPhone, quit with the G8, produced by a one-command repeatable
  script — built without a Mac.
- **Phase 3:** a full singleplayer run completes on the iPhone with the G8 controller, audio plays,
  and a save persists across an app relaunch.
- **Overall:** the entire build → sign → deploy → run loop runs from Linux with a free Apple ID.

## Open decisions (deferred, not blocking)

- Exact deployment target / iPhone model (assume a recent iPhone, iOS 16+, GL ES 3 available).
- Wi-Fi install convenience (Phase 4-ish, optional).
- Multiplayer transport/UPnP specifics on iOS (Phase 4).
- Interactive on-device debugger (optional, after Phase 3).
