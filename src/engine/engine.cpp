#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "renderer/gl_context.h"
#include "renderer/renderer.h"
#include "renderer/debug_draw.h"
#include "renderer/hud.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/collision.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/projectile.h"
#include "core/log.h"
#include "core/math.h"
#include "core/frame_allocator.h"
#include "core/allocation_tracker.h"

#include <glad/glad.h>
#include <cmath>

static FrameAllocator s_frameAllocator;

// ---------------------------------------------------------------------------
// Spawn helpers for test enemies
// ---------------------------------------------------------------------------
static void spawnTestEnemies(EntityPool& pool, const LevelGrid& grid) {
    // Ground enemies in rooms 1,2,3
    struct SpawnInfo { f32 x, z; bool flying; };
    SpawnInfo spawns[] = {
        // Room 1 (13-20, 2-9): 3 ground enemies
        {15.5f, 4.5f, false},
        {17.5f, 6.5f, false},
        {19.0f, 4.0f, false},
        // Room 2 (22-30, 2-8): 2 ground + 1 flyer
        {24.5f, 4.5f, false},
        {26.5f, 5.5f, false},
        {25.5f, 4.5f, true},
        // Room 3 (2-8, 12-20): 2 ground + 1 flyer
        {4.5f, 14.5f, false},
        {6.0f, 17.0f, false},
        {5.0f, 15.5f, true},
        // Room 4 (14-22, 13-19): 2 ground + 2 flyers
        {16.5f, 15.5f, false},
        {19.5f, 16.5f, false},
        {17.5f, 15.0f, true},
        {20.0f, 17.0f, true},
    };

    for (auto& s : spawns) {
        // Get floor height at spawn position
        u32 gx = static_cast<u32>(s.x);
        u32 gz = static_cast<u32>(s.z);
        f32 floorH = 0.0f;
        if (LevelGridSystem::isInBounds(grid, gx, gz) &&
            !LevelGridSystem::isSolid(grid, gx, gz)) {
            floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
        }

        Vec3 halfExtents = s.flying ? Vec3{0.3f, 0.3f, 0.3f} : Vec3{0.4f, 0.5f, 0.4f};
        f32 spawnY = s.flying ? (floorH + 1.5f) : (floorH + halfExtents.y);

        EntitySystem::spawn(pool,
            Vec3{s.x, spawnY, s.z},
            halfExtents,
            s.flying,
            s.flying ? 30.0f : 50.0f,    // health
            s.flying ? 4.0f : 2.5f,       // moveSpeed
            15.0f,                         // detectionRange
            s.flying ? 8.0f : 2.5f,       // attackRange
            s.flying ? 1.5f : 1.0f,       // attackCooldown
            s.flying ? 8.0f : 10.0f       // damage
        );
    }
}

// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------
void Engine::init() {
    Log::init();
    LOG_INFO("Engine initializing...");

    if (!Window::init("DungeonEngine", 1280, 720)) {
        LOG_ERROR("Failed to initialize window");
        return;
    }

    if (!GLContext::init(Window::getHandle())) {
        LOG_ERROR("Failed to initialize GL context");
        return;
    }

    Clock::init();
    Input::init();
    Input::setRelativeMouseMode(true);

    s_frameAllocator.init(1024 * 1024);
    AllocationTracker::init();

    Renderer::init();
    DebugDraw::init();
    HUD::init();

    // Shaders
    m_basicShader = ShaderSystem::load("assets/shaders/basic.vert",
                                       "assets/shaders/basic.frag");
    m_unlitShader = ShaderSystem::load("assets/shaders/unlit.vert",
                                       "assets/shaders/unlit.frag");
    m_wallTexture = TextureSystem::createWhite();
    m_cubeMesh    = MeshSystem::createCube();

    // Build level
    LevelGridSystem::init(m_grid, 32, 32, 1.0f);
    Vec3 spawnPos = LevelGen::generateTestDungeon(m_grid);
    m_sectionCount = LevelMeshSystem::buildAll(m_grid, m_sections, MAX_LEVEL_SECTIONS);

    // Place player
    m_player.position = spawnPos;
    m_player.yaw      = 0.0f;
    m_player.pitch    = 0.0f;

    // Weapons
    initWeaponTable(m_weaponDefs, m_weaponDefCount);
    m_weaponState.currentWeapon = 0;
    m_weaponState.cooldownTimer = 0.0f;

    // Entities
    EntitySystem::init(m_entities);
    spawnTestEnemies(m_entities, m_grid);
    LOG_INFO("Spawned %u enemies", EntitySystem::activeCount(m_entities));

    // Projectiles
    ProjectileSystem::init(m_projectiles);

    m_running     = true;
    m_accumulator = 0.0;
    m_statsTimer  = 0.0;
    m_updateCount = 0;
    m_frameCount  = 0;

    LOG_INFO("Engine initialized — Phase 3 combat ready");
}

void Engine::shutdown() {
    LOG_INFO("Engine shutting down...");

    MeshSystem::destroy(m_cubeMesh);
    LevelMeshSystem::destroyAll(m_sections, m_sectionCount);
    LevelGridSystem::shutdown(m_grid);

    ShaderSystem::destroy(m_basicShader);
    ShaderSystem::destroy(m_unlitShader);
    TextureSystem::destroy(m_wallTexture);

    HUD::shutdown();
    DebugDraw::shutdown();
    Renderer::shutdown();
    s_frameAllocator.shutdown();
    Input::shutdown();
    GLContext::shutdown();
    Window::shutdown();
    AllocationTracker::shutdown();
    Log::shutdown();
}

void Engine::run() {
    while (m_running) {
        Clock::update();
        f64 frameTime = Clock::getDeltaSeconds();

        f64 maxFrameTime = FIXED_DT * MAX_STEPS_PER_FRAME;
        if (frameTime > maxFrameTime) frameTime = maxFrameTime;

        s_frameAllocator.reset();
        AllocationTracker::resetFrameCount();

        Window::pollEvents();
        if (Window::shouldClose()) { m_running = false; break; }

        glViewport(0, 0, Window::getWidth(), Window::getHeight());

        m_accumulator += frameTime;
        while (m_accumulator >= FIXED_DT) {
            Input::update();
            update(static_cast<f32>(FIXED_DT));
            m_accumulator -= FIXED_DT;
            m_updateCount++;
        }

        render(static_cast<f32>(m_accumulator / FIXED_DT));
        m_frameCount++;

        m_statsTimer += frameTime;
        if (m_statsTimer >= 1.0) {
            logStats();
            m_statsTimer  -= 1.0;
            m_updateCount  = 0;
            m_frameCount   = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Update (60 Hz fixed timestep)
// ---------------------------------------------------------------------------
void Engine::update(f32 dt) {
    if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) m_running = false;

    // Toggle debug overlay
    if (Input::isKeyPressed(SDL_SCANCODE_F1)) {
        DebugDraw::setEnabled(!DebugDraw::isEnabled());
    }

    // Toggle noclip
    if (Input::isKeyPressed(SDL_SCANCODE_F2)) {
        m_player.noclip = !m_player.noclip;
        LOG_INFO("Noclip: %s", m_player.noclip ? "ON" : "OFF");
    }

    // Weapon switching: 1/2/3
    for (u32 i = 0; i < m_weaponDefCount && i < 3; i++) {
        if (Input::isKeyPressed(SDL_SCANCODE_1 + i)) {
            m_weaponState.currentWeapon = static_cast<u8>(i);
            m_weaponState.cooldownTimer = 0.0f;
            LOG_INFO("Weapon: %s", m_weaponDefs[i].name);
        }
    }

    // Player movement
    PlayerController::update(m_player, dt);
    if (!m_player.noclip) {
        Collision::moveAndSlide(m_player, m_grid, dt);
    }

    // Soft target lock (middle mouse)
    updateTargetLock(dt);

    // Weapon fire
    handleWeaponFire(dt);

    // Enemy AI
    EnemyAI::update(m_entities, m_grid, m_player, m_projectiles, dt);

    // Projectiles
    ProjectileSystem::update(m_projectiles, m_grid, m_entities, m_player, dt);

    // Entity timers (flash, death despawn)
    EntitySystem::tickTimers(m_entities, dt);

    // Player damage flash decay
    if (m_player.damageFlashTimer > 0.0f)
        m_player.damageFlashTimer -= dt;

    // Hit marker decay
    if (m_hitMarkerTimer > 0.0f)
        m_hitMarkerTimer -= dt;

    // Camera recoil decay
    m_weaponState.recoilOffset *= 0.85f;

    // Apply camera from player (with recoil)
    PlayerController::applyToCamera(m_player, m_camera);
    m_camera.pitch += m_weaponState.recoilOffset;

    // Player-enemy push collision (prevent walking through enemies)
    AABB playerBox = {
        m_player.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
        m_player.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
    };
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = m_entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        AABB entBox = entityAABB(e);
        if (CombatQuery::aabbOverlap(playerBox, entBox)) {
            // Push player out along XZ shortest axis
            Vec3 toPlayer = m_player.position - e.position;
            f32 pushX = (e.halfExtents.x + PLAYER_HALF_WIDTH) - fabsf(toPlayer.x);
            f32 pushZ = (e.halfExtents.z + PLAYER_HALF_WIDTH) - fabsf(toPlayer.z);
            if (pushX > 0.0f && pushZ > 0.0f) {
                if (pushX < pushZ) {
                    m_player.position.x += (toPlayer.x > 0) ? pushX : -pushX;
                } else {
                    m_player.position.z += (toPlayer.z > 0) ? pushZ : -pushZ;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Weapon fire
// ---------------------------------------------------------------------------
void Engine::handleWeaponFire(f32 dt) {
    m_weaponState.cooldownTimer -= dt;
    if (m_weaponState.cooldownTimer < 0.0f)
        m_weaponState.cooldownTimer = 0.0f;

    if (!Input::isMouseButtonDown(SDL_BUTTON_LEFT)) return;
    if (m_weaponState.cooldownTimer > 0.0f) return;

    const WeaponDef& wpn = m_weaponDefs[m_weaponState.currentWeapon];
    m_weaponState.cooldownTimer = wpn.cooldown;

    Vec3 eyePos = m_player.position + Vec3{0, m_player.eyeHeight, 0};
    Vec3 forward = normalize(Vec3{
        -sinf(m_player.yaw) * cosf(m_player.pitch),
         sinf(m_player.pitch),
        -cosf(m_player.yaw) * cosf(m_player.pitch)
    });

    AttackResult result;

    switch (wpn.type) {
    case WeaponType::MELEE:
        result = Combat::fireMelee(wpn, eyePos, forward, m_entities);
        break;
    case WeaponType::HITSCAN:
        result = Combat::fireHitscan(wpn, eyePos, forward, m_grid, m_entities);
        // Store for debug viz
        if (result.hitEntity || result.hitWorld) {
            m_lastCombatHit.hit      = true;
            m_lastCombatHit.position = result.hitPosition;
            m_lastCombatHit.normal   = result.hitNormal;
            m_lastCombatHit.distance = result.hitDistance;
            m_lastCombatHit.type     = result.hitEntity ? CombatHit::ENTITY : CombatHit::WORLD;
        }
        break;
    case WeaponType::PROJECTILE:
        Combat::fireProjectile(wpn, eyePos, forward, m_projectiles);
        result.didFire = true;
        break;
    }

    // Camera recoil
    m_weaponState.recoilOffset += wpn.recoilKick;

    // Hit marker
    if (result.hitEntity) {
        m_hitMarkerTimer = 0.2f;
    }
}

// ---------------------------------------------------------------------------
// Soft target lock
// ---------------------------------------------------------------------------
void Engine::updateTargetLock(f32 dt) {
    Vec3 eyePos = m_player.position + Vec3{0, m_player.eyeHeight, 0};
    Vec3 forward = normalize(Vec3{
        -sinf(m_player.yaw) * cosf(m_player.pitch),
         sinf(m_player.pitch),
        -cosf(m_player.yaw) * cosf(m_player.pitch)
    });

    if (Input::isMouseButtonDown(SDL_BUTTON_MIDDLE)) {
        // Find or maintain target
        if (!m_player.lockActive) {
            // Acquire target: nearest enemy in 30-degree cone
            EntityHandle hits[8];
            f32 distances[8];
            f32 coneCos = cosf(radians(30.0f));
            u32 count = CombatQuery::queryConeSorted(m_entities, eyePos, forward,
                                                      coneCos, 30.0f,
                                                      hits, distances, 8);
            if (count > 0) {
                m_player.lockIndex      = hits[0].index;
                m_player.lockGeneration = hits[0].generation;
                m_player.lockActive     = true;
                m_player.lockLosTimer   = 0.0f;
            }
        }

        if (m_player.lockActive) {
            EntityHandle h = {m_player.lockIndex, m_player.lockGeneration};
            Entity* target = handleGet(m_entities, h);

            if (!target || (target->flags & ENT_DEAD)) {
                // Target gone
                m_player.lockActive = false;
            } else {
                Vec3 toTarget = target->position - eyePos;
                f32 dist = length(toTarget);

                // Break if too far or too far off-screen
                if (dist > 40.0f) {
                    m_player.lockActive = false;
                } else if (dist > 0.001f) {
                    Vec3 dirToTarget = toTarget * (1.0f / dist);
                    f32 d = dot(dirToTarget, forward);
                    if (d < cosf(radians(45.0f))) {
                        m_player.lockActive = false;
                    } else {
                        // Soft aim bias: gently steer toward target
                        f32 lockStrength = 0.05f;
                        Vec3 biased = normalize(forward + (dirToTarget - forward) * lockStrength);

                        m_player.yaw   = atan2f(-biased.x, -biased.z);
                        m_player.pitch = asinf(biased.y);
                    }
                }
            }
        }
    } else {
        m_player.lockActive = false;
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void Engine::render(f32 alpha) {
    (void)alpha;

    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    f32 aspect = static_cast<f32>(Window::getWidth()) /
                 static_cast<f32>(Window::getHeight());
    CameraSystem::computeMatrices(m_camera, aspect);

    Renderer::beginFrame(m_camera);
    Renderer::setDirectionalLight(
        normalize(Vec3{-0.3f, -1.0f, -0.5f}),
        {1.0f, 0.95f, 0.9f},
        {0.15f, 0.15f, 0.2f}
    );

    // Level geometry
    LevelMeshSystem::submitAll(m_sections, m_sectionCount,
                               m_basicShader, m_wallTexture);

    // Entities
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = m_entities.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;

        // Dying: shrink Y
        f32 scaleY = 1.0f;
        if (e.flags & ENT_DEAD) {
            scaleY = (e.deathTimer > 0.0f) ? e.deathTimer : 0.01f;
        }

        Vec3 renderHalf = e.halfExtents;
        renderHalf.y *= scaleY;
        Vec3 renderPos = e.position;
        if (e.flags & ENT_DEAD) {
            renderPos.y -= e.halfExtents.y * (1.0f - scaleY);
        }

        Mat4 model = Mat4::translate(renderPos)
                   * Mat4::rotateY(e.yaw)
                   * Mat4::scale(renderHalf * 2.0f);
        AABB bounds = {renderPos - renderHalf, renderPos + renderHalf};

        // Flash: use unlit shader with red tint when hit
        if (e.flashTimer > 0.0f) {
            f32 flash = e.flashTimer / 0.12f;
            Vec4 flashColor = {1.0f, 0.3f * flash, 0.3f * flash, 1.0f};
            Renderer::submit(m_unlitShader, m_wallTexture, m_cubeMesh, model, bounds, flashColor);
        } else {
            // Flying = blue-ish, ground = brownish
            Vec4 entColor = (e.flags & ENT_FLYING)
                ? Vec4{0.4f, 0.5f, 1.0f, 1.0f}
                : Vec4{0.8f, 0.5f, 0.3f, 1.0f};
            Renderer::submit(m_unlitShader, m_wallTexture, m_cubeMesh, model, bounds, entColor);
        }
    }

    // Projectiles
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile& p = m_projectiles.projectiles[i];
        if (!p.active) continue;

        Mat4 model = Mat4::translate(p.position)
                   * Mat4::scale({p.radius * 2.0f, p.radius * 2.0f, p.radius * 2.0f});
        AABB bounds = {
            p.position - Vec3{p.radius, p.radius, p.radius},
            p.position + Vec3{p.radius, p.radius, p.radius}
        };

        Vec4 projColor = p.fromPlayer
            ? Vec4{1.0f, 0.5f, 0.1f, 1.0f}
            : Vec4{0.8f, 0.2f, 1.0f, 1.0f};
        Renderer::submit(m_unlitShader, m_wallTexture, m_cubeMesh, model, bounds, projColor);
    }

    Renderer::flush();

    // --- Debug overlay (F1) ---
    DebugDraw::clear();
    if (DebugDraw::isEnabled()) {
        // Player AABB
        Vec3 feet = m_player.position;
        AABB playerBox = {
            feet + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
            feet + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
        };
        Vec3 boxColor = m_player.onGround ? Vec3{0,1,0} : Vec3{1,1,0};
        DebugDraw::box(playerBox, boxColor);

        // Enemy AABBs
        for (u32 i = 0; i < MAX_ENTITIES; i++) {
            const Entity& e = m_entities.entities[i];
            if (!(e.flags & ENT_ACTIVE)) continue;
            Vec3 c = (e.flags & ENT_DEAD) ? Vec3{0.5f,0.5f,0.5f}
                   : (e.flags & ENT_FLYING) ? Vec3{0.3f,0.3f,1.0f}
                   : Vec3{1.0f,0.3f,0.3f};
            DebugDraw::box(entityAABB(e), c);
        }

        // Last combat hit
        if (m_lastCombatHit.hit) {
            Vec3 eyePos = m_player.position + Vec3{0, m_player.eyeHeight, 0};
            DebugDraw::line(eyePos, m_lastCombatHit.position, {1,0,0});
            DebugDraw::cross(m_lastCombatHit.position, 0.15f, {1,0.5f,0});
            DebugDraw::ray(m_lastCombatHit.position, m_lastCombatHit.normal, 0.5f, {1,1,0});
        }
    }

    // Target lock indicator (always visible, not just F1)
    if (m_player.lockActive) {
        EntityHandle h = {m_player.lockIndex, m_player.lockGeneration};
        Entity* target = handleGet(m_entities, h);
        if (target) {
            AABB lockBox = entityAABB(*target);
            // Expand slightly
            lockBox.min = lockBox.min - Vec3{0.05f, 0.05f, 0.05f};
            lockBox.max = lockBox.max + Vec3{0.05f, 0.05f, 0.05f};
            // Temporarily force debug lines on for lock indicator
            bool wasEnabled = DebugDraw::isEnabled();
            DebugDraw::setEnabled(true);
            DebugDraw::box(lockBox, {0.0f, 1.0f, 1.0f}); // cyan
            DebugDraw::setEnabled(wasEnabled);
        }
    }

    DebugDraw::flush(m_camera.viewProjection);

    // --- HUD ---
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    // Crosshair (white normally, red during damage flash)
    Vec3 crossColor = (m_player.damageFlashTimer > 0.0f)
                    ? Vec3{1.0f, 0.3f, 0.3f}
                    : Vec3{1.0f, 1.0f, 1.0f};
    HUD::drawCrosshair(sw, sh, crossColor);

    // Hit marker
    if (m_hitMarkerTimer > 0.0f) {
        HUD::drawHitMarker(sw, sh, m_hitMarkerTimer / 0.2f);
    }

    // Health bar
    HUD::drawHealthBar(sw, sh, m_player.health, m_player.maxHealth);

    // Weapon indicator
    HUD::drawWeaponIndicator(sw, sh, m_weaponState.currentWeapon);

    GLContext::swapBuffers(Window::getHandle());
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
void Engine::logStats() {
    f64 avgFrameTime = (m_frameCount > 0) ? (1000.0 / m_frameCount) : 0.0;

    LOG_INFO("FPS: %u | Frame: %.2f ms | Draw: %u | Vis: %u | Ent: %u | Proj: %u | HP: %.0f",
             m_frameCount,
             avgFrameTime,
             Renderer::getDrawCallCount(),
             Renderer::getVisibleCount(),
             EntitySystem::activeCount(m_entities),
             m_projectiles.activeCount,
             m_player.health);
}
