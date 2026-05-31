#include <doctest/doctest.h>
#include "net/pending_skill_ring.h"

TEST_CASE("PendingSkillRing: empty") {
    PendingSkillRing r;
    PendingSkillRingOps::reset(r);
    CHECK(r.count == 0);
    // ack on empty ring returns false
    CHECK(PendingSkillRingOps::ack(r, 100, 0) == false);
}

TEST_CASE("PendingSkillRing: record") {
    PendingSkillRing r;
    PendingSkillRingOps::reset(r);
    PendingSkillRingOps::record(r, 100, 0);
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 100);
    CHECK(r.entries[0].skillSlot == 0);
    // Record a second entry with a sentinel slot (boot skill)
    PendingSkillRingOps::record(r, 101, PENDING_SKILL_SLOT_BOOT);
    CHECK(r.count == 2);
    CHECK(r.entries[1].skillSlot == PENDING_SKILL_SLOT_BOOT);
}

TEST_CASE("PendingSkillRing: ack removes entry") {
    PendingSkillRing r;
    PendingSkillRingOps::reset(r);
    PendingSkillRingOps::record(r, 100, 1);
    PendingSkillRingOps::record(r, 101, PENDING_SKILL_SLOT_HELMET);
    // ack the first entry
    CHECK(PendingSkillRingOps::ack(r, 100, 1) == true);
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 101);
    CHECK(r.entries[0].skillSlot == PENDING_SKILL_SLOT_HELMET);
    // ack a non-existent entry returns false
    CHECK(PendingSkillRingOps::ack(r, 999, 0) == false);
    CHECK(r.count == 1);
}

TEST_CASE("PendingSkillRing: expireOlderThan") {
    PendingSkillRing r;
    PendingSkillRingOps::reset(r);
    PendingSkillRingOps::record(r, 50,  0);
    PendingSkillRingOps::record(r, 150, 1);
    PendingSkillRingOps::record(r, 200, PENDING_SKILL_SLOT_BOOT);
    // Expire everything with clientTick < 100 — only tick 50 should go
    PendingSkillRingOps::expireOlderThan(r, 100);
    CHECK(r.count == 2);
    CHECK(r.entries[0].clientTick == 150);
    CHECK(r.entries[1].clientTick == 200);
    // Expire everything with clientTick < 300 — both remaining entries go
    PendingSkillRingOps::expireOlderThan(r, 300);
    CHECK(r.count == 0);
}
