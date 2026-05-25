# New boss limb config — code snippet (case B, only if no existing config fits)

Configs 0–6 already exist (default legs; spider legs; tentacles; spikes; blades+hidden legs;
lich sleeves+hidden legs; Nyx arms+hidden legs). **Reuse one if you can.** Add a new config only
for a genuinely new limb arrangement. All edits are in `src/game/limb_system.cpp`.

`LimbDef` / `LimbConfig` shapes (from `src/game/limb_system.h`):
```cpp
struct LimbDef {
    Vec3 pivotOffset;   // relative to entity feet
    Vec3 meshHalfSize;  // box half-extents for the limb
    f32  restAngle;     // resting pose (radians)
    u8   pivotAxis;     // 0=X(pitch) 1=Y(yaw) 2=Z(roll)
    bool mirrored;      // negate angle for the paired limb
};
struct LimbConfig { u8 limbCount; LimbDef limbs[MAX_LIMBS]; };  // MAX_LIMBS = 12
```

## 1. Define the config (~line 300, beside the other `s_boss*Config`)

```cpp
// Config 7: <Boss> — base legs (2) + <your extra limbs>
static const LimbConfig s_boss<Name>Config = {
    4,
    {
        SKEL_BASE_LIMBS,                                                   // slots 0-1 = legs
        {{ 0.25f, 0.75f, -0.05f}, {0.08f, 0.32f, 0.08f}, -0.5f, 0, false}, // left
        {{-0.25f, 0.75f, -0.05f}, {0.08f, 0.32f, 0.08f}, -0.5f, 0, true},  // right (mirrored)
    }
};
```

## 2. Register it in `getBossConfig()` (~line 388)

```cpp
    case 7: return s_boss<Name>Config;
```

## 3. If legs should be HIDDEN (robed/floating body)

`getBossLimbMeshId()` (~line 399) returns mesh 0 for leg slots when `configId` is 4/5/6. If your
config also hides legs, add its id to that check:
```cpp
    if (limbIdx < 2) {
        if (configId == 4 || configId == 5 || configId == 6 || configId == 7) return 0; // hide legs
        return s_legMeshId;
    }
```
For the extra (≥2) limb slots it returns `s_spiderLegMeshId` for config 1 else `s_armMeshId` —
extend if your limbs need a different mesh.

## 4. Wire it up

Set `"limbConfig": 7` in the boss's bosses.json entry, then build assets + compile.
Add the new config to the boss-limb-config notes in the `engine-reference` skill.
