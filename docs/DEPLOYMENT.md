# Deployment Runbook (for the release agent)

This is the single source of truth for **where each build artifact goes** when shipping DungeonEngine.
Read it top to bottom before a release. It assumes you are an agent with shell access to this repo and
the credentials listed in §6.

---

## 0. Mental model (read this first)

There are **three delivery targets**, and they are produced/uploaded differently:

| Target | Built by | Uploaded by | Store? |
|---|---|---|---|
| **Steam** | CI (`steam` variant, `-DUSE_STEAM=ON`) | **You**, via `steamcmd` | yes — App ID **4819550** |
| **itch.io** | CI (`itch` variant, default Steam-free build) | **You**, via `butler` | yes — DRM-free |
| **Nintendo Switch** | `docker` + devkitPro (local) | `nxlink` over LAN | no — homebrew `.nro` |

**Golden rule of "what goes where":**
- Zips named `DungeonEngine-steam-*` → **Steam only**.
- Zips named `DungeonEngine-itch-*` → **itch only**.
- Never cross them. The `steam` zips bundle `libsteam_api` and expect the Steam client; the `itch` zips
  are Steam-free and DRM-free. Shipping a `steam` build on itch means it won't run without Steam;
  shipping an `itch` build on Steam loses relay networking + matchmaking.

CI builds everything on a `v*` tag and attaches the zips to a GitHub Release. **CI does not push to any
store.** Your job is: cut the tag (or find the Release), download the zips, and fan them out to Steam +
itch. Switch is fully separate and out-of-band.

---

## 1. PLACEHOLDERS you must fill before deploying

These are the only values not derivable from the repo. Get them once and record them here (or in your
own secure notes — do **not** commit credentials):

| Placeholder | What it is | Where to find it |
|---|---|---|
| `<STEAM_BUILD_ACCOUNT>` | Steam account with "Edit App Metadata / publish" on App 4819550 | Steamworks → Users & Permissions |
| `<WINDOWS_DEPOT_ID>` | Steam depot for the Windows build | Steamworks → App Admin → **Depots** |
| `<LINUX_DEPOT_ID>` | Steam depot for the Linux build | same |
| `<MACOS_DEPOT_ID>` | Steam depot for the macOS build | same |
| `<ITCH_TARGET>` | itch project, form `user/game` (e.g. `edrethardo/curse-of-the-dungeon-engine`) | itch.io project URL |
| `<SWITCH_IP>` | LAN IP of the dev console | router / `deploy_switch.sh` default is `192.168.2.54` |

Known-fixed values (do **not** change): Steam **App ID = 4819550**; org/app = `EdRethardo` /
`DungeonEngine` (see §5 note).

---

## 2. Cut a build → get the artifacts

The release is driven entirely by git tags. Use the helper:

```bash
tools/release.sh patch --push        # bump patch, tag v<X.Y.Z>, push -> CI builds + publishes
# or:  tools/release.sh 1.3.0 --push  # explicit version
```

`release.sh` refuses to run with a dirty tree, bumps the version in `src/CMakeLists.txt` (Switch NACP)
and `.github/workflows/build.yml` (macOS `CFBundleVersion`), commits `chore: release v<X.Y.Z>`, tags,
and pushes. Pushing the `v*` tag triggers `.github/workflows/build.yml`.

**Preconditions for a Steam-inclusive CI run:** the repo secret **`STEAMWORKS_SDK_URL`** must be set (a
private URL to `steamworks_sdk_*.zip`). Without it, the `steam` matrix jobs fail with a clear error and
only the `itch` jobs succeed. The `itch` jobs never need it.

When CI finishes, the GitHub Release `v<X.Y.Z>` holds **8 zips**:

```
DungeonEngine-itch-Windows.zip        DungeonEngine-steam-Windows.zip
DungeonEngine-itch-Linux-22.04.zip    DungeonEngine-steam-Linux-22.04.zip
DungeonEngine-itch-Linux-24.04.zip    DungeonEngine-steam-Linux-24.04.zip
DungeonEngine-itch-macOS.zip          DungeonEngine-steam-macOS.zip
```

Download them:

```bash
mkdir -p dist && cd dist
gh release download "v<X.Y.Z>" --pattern 'DungeonEngine-*.zip'
```

**Which Linux to ship:** prefer **`Linux-22.04`** as the primary for both stores (older glibc = widest
compatibility, matches the Steam Linux runtime). Ship `24.04` only as an extra channel if you
specifically need it. Don't put both on the same channel/depot.

---

## 3. Steam (App ID 4819550) — via `steamcmd`

Use only the `DungeonEngine-steam-*` zips.

### 3a. Stage the content (one folder per OS depot)
```bash
cd dist
rm -rf steam_content && mkdir -p steam_content/{windows,linux,macos}
unzip -o DungeonEngine-steam-Windows.zip      -d steam_content/windows
unzip -o DungeonEngine-steam-Linux-22.04.zip  -d steam_content/linux
unzip -o DungeonEngine-steam-macOS.zip        -d steam_content/macos
```
**Do NOT add `steam_appid.txt`** to any depot — the packaging step already strips it, and the live Steam
client injects the real App ID at launch. A stray `steam_appid.txt` would mask it. (`tools/steam_appid.txt`
exists only for *local dev* runs of the binary.)

### 3b. Write the build script (`dist/app_build.vdf`)
Fill the depot IDs from §1. `contentroot` is relative to the VDF file.
```
"appbuild"
{
  "appid"       "4819550"
  "desc"        "v<X.Y.Z>"
  "buildoutput" "./steam_output"
  "contentroot" "./steam_content"
  "setlive"     "beta"          // upload to the BETA branch; promote to default on the partner site (§3d)
  "depots"
  {
    "<WINDOWS_DEPOT_ID>" { "FileMapping" { "LocalPath" "./windows/*" "DepotPath" "." "recursive" "1" } }
    "<LINUX_DEPOT_ID>"   { "FileMapping" { "LocalPath" "./linux/*"   "DepotPath" "." "recursive" "1" } }
    "<MACOS_DEPOT_ID>"   { "FileMapping" { "LocalPath" "./macos/*"   "DepotPath" "." "recursive" "1" } }
  }
}
```

### 3c. Upload
```bash
cd dist
steamcmd +login "<STEAM_BUILD_ACCOUNT>" +run_app_build "$(pwd)/app_build.vdf" +quit
```
First run prompts for Steam Guard (2FA). On CI/headless, pre-seed the `steamcmd` config or use a build
account with a shared-secret TOTP — never hardcode credentials in the repo.

### 3d. Go live
`setlive "beta"` above publishes to the **beta** branch only. Verify on that branch, then promote to the
**default** branch from **Steamworks → SteamPipe → Builds → set build live on `default`**. Promoting by
hand (not `setlive "default"` in the VDF) is the safety gate — it prevents a bad build going straight to
the public.

### 3e. One-time partner-site config (already-known requirements)
These are configured once on Steamworks and must stay in sync with the code:
- **Steam Cloud (saves):** exactly as specified in [docs/steam_cloud.md](steam_cloud.md) — Auto-Cloud
  root mappings + the `save_*.dat`, `difficulty_unlock.dat`, `controls.json`, `audio.json` patterns
  (and **not** `video.cfg`). No SDK involved; the game just writes to `SDL_GetPrefPath`.
- **Matchmaking / relay:** the app must have **P2P / Steam Datagram Relay** enabled for the relay
  transport + lobbies to work (App Admin → networking). Without it, invites/join/browse fail at runtime.
- **Launch options / depots** must exist for each OS (per-OS launch option pointing at `DungeonEngine[.exe]`).
- **Publish** the Steamworks changes — Cloud/Auto-Cloud/networking settings only go live after publishing.

---

## 4. itch.io — via `butler`

Use only the `DungeonEngine-itch-*` zips. `butler` diffs against the previous push, so **push extracted
folders, not the zip** (gives players patch-sized updates, not full re-downloads).

### 4a. Stage
```bash
cd dist
rm -rf itch_content && mkdir -p itch_content/{windows,linux,mac}
unzip -o DungeonEngine-itch-Windows.zip      -d itch_content/windows
unzip -o DungeonEngine-itch-Linux-22.04.zip  -d itch_content/linux
unzip -o DungeonEngine-itch-macOS.zip        -d itch_content/mac
```

### 4b. Push (channel names are the platform)
```bash
butler push itch_content/windows "<ITCH_TARGET>:windows" --userversion "<X.Y.Z>"
butler push itch_content/linux   "<ITCH_TARGET>:linux"   --userversion "<X.Y.Z>"
butler push itch_content/mac     "<ITCH_TARGET>:osx"     --userversion "<X.Y.Z>"
```
`butler` auto-tags platform compatibility from the channel name (`windows`/`linux`/`osx`). Verify with
`butler status "<ITCH_TARGET>"`.

**Auth:** `butler login` once (opens a browser), or set `BUTLER_API_KEY` in the environment for headless
runs. Never commit the key.

---

## 5. Nintendo Switch — homebrew `.nro` (out-of-band, no store)

Switch is **not** a store release and **never** uses Steam (`USE_STEAM` is hard-blocked on Switch by
`external/CMakeLists.txt`). It's a devkitPro homebrew build deployed to a dev console over LAN.

### Preconditions
- Docker installed; the `devkitpro/devkita64` image pulled (`docker pull devkitpro/devkita64`).
- `build-switch/` configured once:
  `docker run --rm -u $(id -u):$(id -g) -v $(pwd):/game -w /game devkitpro/devkita64 bash -c \
   "source /opt/devkitpro/switchvars.sh && cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake -DCMAKE_BUILD_TYPE=Release"`
- The console is on the **homebrew menu** (netloader listening) and reachable at `<SWITCH_IP>`.

### Build + deploy
```bash
./tools/deploy_switch.sh                 # uses the DHCP-reserved default IP (192.168.2.54)
SWITCH_IP=<SWITCH_IP> ./tools/deploy_switch.sh   # one-off override
```
This runs the docker devkitPro build (produces `build-switch/DungeonEngine.nro`, ~62 MB, assets bundled
into RomFS) and `nxlink`s it to the console. To **build without deploying** (e.g. CI-less verification),
run the docker `cmake --build build-switch` line without the `nxlink` step.

---

## 6. Credentials & secrets — what lives where

| Secret | Where it goes | Never |
|---|---|---|
| `STEAMWORKS_SDK_URL` | GitHub → repo **Settings → Secrets → Actions** | commit the SDK / URL |
| Steam build login + Steam Guard | local `steamcmd` config / secure runner secret | hardcode in repo or VDF |
| `BUTLER_API_KEY` | env var on the deploy machine | commit |
| Steamworks SDK (`external/steamworks/`) | local only, **gitignored** | commit (proprietary) |

---

## 7. Pre-flight checklist

- [ ] Working tree clean; `tools/release.sh` version bump committed + tagged.
- [ ] `STEAMWORKS_SDK_URL` secret present (else the `steam` CI jobs fail).
- [ ] GitHub Release `v<X.Y.Z>` has all 8 zips (CI green on windows/linux×2/macos × itch/steam).
- [ ] Steam: `steam` zips only; **no `steam_appid.txt`** in any depot; uploaded to **beta** first.
- [ ] itch: `itch` zips only; pushed as extracted folders to `windows`/`linux`/`osx`.
- [ ] Steam partner-site: Cloud + Auto-Cloud + P2P/SDR enabled and **published** (§3e).

## 8. Post-deploy verification

- **Steam (beta branch):** install from the beta branch on a clean machine → boots, saves sync
  (Auto-Cloud), and Host Online / invite / join / browser work (relay). Then promote to `default` (§3d).
- **itch:** install via the itch app on each platform → boots and runs Steam-free (no `libsteam_api`
  errors, no Steam client required).
- **Switch:** launches from the homebrew menu; matchmaking menu rows are hidden (Steam is off), LAN/ENet
  networking still works.
- **macOS (both stores):** first-launch Gatekeeper — the bundled `README-macOS.txt` tells players to
  `xattr -cr` the app (it's ad-hoc signed, not Developer-ID). Expected, not a bug.
