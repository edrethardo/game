// hud_journal.cpp — the QUESTS page of the tabbed character menu, in the Diablo 2 mould.
//
// Diablo 2's quest log is an act-tabbed page where each quest is a MARKER plus a name, and picking
// one fills a reading pane with who asked, why, and what is left to do. This is that, with the
// markers drawn as diamonds rather than D2's sockets because the HUD's primitives are lines and a
// diamond stays countable at icon size where a small circle smudges — the same rule the overworld
// minimap's glyph block already follows: SHAPE carries what, COLOUR carries which.
//
// It replaced a dark box floated over the middle of the inventory screen, which overlapped the
// equipment column, showed the live dungeon around its edges, and stated quest state only as the
// colour of a line of text. Text is word-WRAPPED against the pane width — Engine::addChatMessage
// formats into a fixed buffer and was truncating nine of the ten quest blurbs mid-sentence, which
// is what made a real page necessary in the first place.
//
// Geometry comes entirely from InventoryUI::journalLayout / menuFrameLayout — this file never
// re-derives a rect, which is what keeps the clickable rows under the drawn ones.
#include "renderer/hud.h"
#include "renderer/hud_internal.h"   // fillRect/pushLine/pushQuad/flushHUD
#include "renderer/font.h"
#include "game/inventory_ui.h"
#include "game/quest_def.h"
#include "game/quest_state.h"

#include <cstdio>   // snprintf for the objective rows

namespace {

// Colour by state. Gold reads as "done" everywhere else in this game (legendary items, the
// completed-tab edge), so it means done here too.
Vec3 stateColour(Quest::State s) {
    switch (s) {
        case Quest::State::COMPLETE: return Vec3{1.00f, 0.85f, 0.35f};   // gold
        case Quest::State::ACTIVE:   return Vec3{0.95f, 0.95f, 0.95f};   // white
        case Quest::State::OFFERED:  return Vec3{0.70f, 0.76f, 0.88f};   // grey-blue
        default:                     return Vec3{0.38f, 0.38f, 0.42f};   // dim: not yet known
    }
}

// The quest marker: a diamond whose FILL carries progress, left of the name.
//
//   LOCKED   a small dim outline           — "there is something here you have not met"
//   OFFERED  a full-size outline           — known, untouched
//   ACTIVE   outline + centre pip          — under way
//   COMPLETE a SOLID diamond               — done
//
// Fill is the load-bearing cue, not colour: the four states must be distinguishable on the
// Switch's smaller screen and by a player who cannot separate gold from white.
void drawQuestMarker(f32 cx, f32 cy, f32 rad, Quest::State st, Vec3 col) {
    const f32 r = (st == Quest::State::LOCKED) ? rad * 0.55f : rad;

    if (st == Quest::State::COMPLETE) {
        // Solid: scanline the diamond by shrinking the half-width toward the tips. Drawn as lines
        // rather than a textured quad because everything else on this page is, and mixing the two
        // would put this marker in a different batch from the outline states beside it.
        for (f32 dy = -r; dy <= r; dy += 1.0f) {
            const f32 t  = 1.0f - ((dy < 0.0f ? -dy : dy) / r);
            const f32 hw = r * t;
            pushLine(cx - hw, cy + dy, cx + hw, cy + dy, col);
        }
        return;
    }

    pushLine(cx, cy + r, cx + r, cy, col);
    pushLine(cx + r, cy, cx, cy - r, col);
    pushLine(cx, cy - r, cx - r, cy, col);
    pushLine(cx - r, cy, cx, cy + r, col);

    if (st == Quest::State::ACTIVE) {
        const f32 p = r * 0.32f;
        for (f32 dy = -p; dy <= p; dy += 1.0f) pushLine(cx - p, cy + dy, cx + p, cy + dy, col);
    }
}

// Draw `text` wrapped to `maxW`, starting at (x, topY) and descending. Returns the Y below the
// last line so the caller can keep stacking. Breaks on spaces only; a single word longer than the
// column overflows rather than being split, which never happens with authored prose and keeps this
// simple enough to be obviously correct.
f32 drawWrapped(u32 sw, u32 sh, f32 x, f32 topY, f32 maxW,
                const char* text, Vec3 col, f32 scale, f32 lineH) {
    if (!text || !text[0]) return topY;

    char line[256];
    u32  len = 0;
    f32  y   = topY;

    const char* word = text;
    while (*word) {
        const char* end = word;
        while (*end && *end != ' ') end++;
        const u32 wlen = static_cast<u32>(end - word);

        // Would appending this word overflow the column? Both buffers are hard-bounded on every
        // copy, so an unbounded narration truncates a line rather than smashing the stack.
        char probe[256];
        u32  plen = 0;
        for (u32 i = 0; i < len && plen < sizeof(probe) - 1; i++) probe[plen++] = line[i];
        if (len && plen < sizeof(probe) - 1) probe[plen++] = ' ';
        for (u32 i = 0; i < wlen && plen < sizeof(probe) - 1; i++) probe[plen++] = word[i];
        probe[plen] = '\0';

        if (len && FontSystem::textWidth(probe, scale) > maxW) {
            line[len] = '\0';
            FontSystem::drawText(sw, sh, x, y, line, col, scale);
            y -= lineH;
            len = 0;
            for (u32 i = 0; i < wlen && len < sizeof(line) - 1; i++) line[len++] = word[i];
        } else {
            len = 0;
            for (u32 i = 0; i < plen && len < sizeof(line) - 1; i++) line[len++] = probe[i];
        }

        // `word` always advances past the token and then past its trailing spaces, so a word wider
        // than the whole column costs one over-long line and the loop still terminates.
        word = end;
        while (*word == ' ') word++;
    }
    if (len) {
        line[len] = '\0';
        FontSystem::drawText(sw, sh, x, y, line, col, scale);
        y -= lineH;
    }
    return y;
}

} // namespace

void HUD::drawQuestLog(u32 sw, u32 sh, const Quest::Progress& prog,
                       u8 selectedRow, u8 actTab,
                       u8 liveQuestIdx, u16 liveRemaining, u16 liveTotal,
                       s32 mouseX, s32 mouseY) {
    const InventoryUI::JournalRects   r = InventoryUI::journalLayout(sw, sh);
    const InventoryUI::MenuFrameRects m = InventoryUI::menuFrameLayout(sw, sh);
    const InventoryUI::SlotHit hover    = InventoryUI::hitTestJournal(sw, sh, mouseX, mouseY);
    const f32 s  = r.uiScale;
    const f32 tx = 1.0f * s;   // body text scale

    // No opaque backing of its own any more: the menu frame already painted one for every page.
    // The old panel had to draw its own, because it floated over a screen that had none.

    // --- Act tabs ------------------------------------------------------------------------------
    // Deliberately flatter and cooler than the page tabs above them — two rows of identical-looking
    // tabs would read as one confused strip. These are content; those are chrome.
    // Fills first, flushed, THEN labels — HUD lines are batched and would otherwise paint over
    // the immediate-mode text. See hud_menu_frame.cpp for the full note; it cost this screen every
    // one of its tab labels the first time round.
    for (u32 t = 0; t < InventoryUI::JOURNAL_TABS; t++) {
        const f32 x0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);
        const f32 x1 = x0 + r.tabW;
        const bool on = (t == actTab);
        fillRect(x0, r.tabY, x1, r.tabY + r.tabH,
                 on ? Vec3{0.115f, 0.105f, 0.075f} : Vec3{0.038f, 0.038f, 0.050f});
        pushQuad(x0, r.tabY, x1, r.tabY + r.tabH,
                 on ? Vec3{0.62f, 0.54f, 0.30f} : Vec3{0.20f, 0.20f, 0.26f});
    }

    // --- Divider between the list and the reading pane -----------------------------------------
    // (still line work — the single flush below covers the tabs, this rule, the row plates and
    // the quest markers, after which every label on the page is drawn.)
    {
        const f32 dx  = r.detailX - 13.0f * s;
        const f32 top = r.listTopY + 4.0f * s;
        const f32 bot = m.contentY + 4.0f * s;
        pushLine(dx, bot, dx, top, Vec3{0.20f, 0.19f, 0.16f});
    }

    // --- Quest list for the visible act ---------------------------------------------------------
    // The rows are the act's quests IN TABLE ORDER, which is the road's own walking order.
    u8 rowQuest[InventoryUI::JOURNAL_ROWS];
    u8 rowCount = 0;
    for (u32 i = 0; i < Quest::COUNT && rowCount < InventoryUI::JOURNAL_ROWS; i++) {
        if (Quest::actOf(Quest::QUESTS[i].zoneFloor) != actTab + 1) continue;
        rowQuest[rowCount++] = static_cast<u8>(i);
    }

    u8 doneCount = 0;
    for (u8 row = 0; row < rowCount; row++)
        if (prog.state[rowQuest[row]] == static_cast<u8>(Quest::State::COMPLETE)) doneCount++;

    for (u8 row = 0; row < rowCount; row++) {
        const u8 q = rowQuest[row];
        const Quest::State st = static_cast<Quest::State>(prog.state[q]);
        // listTopY is row 0's TOP edge and rows DESCEND, so row i's BOTTOM edge is
        // listTopY - rowH*(i+1) — exactly what hitTestJournal's floor(dy/rowH) inverts. Placing the
        // baseline off by one rowH here is the draw/hit-test drift the layout split exists to stop.
        const f32 rowBot = r.listTopY - r.rowH * (static_cast<f32>(row) + 1.0f);
        const f32 y      = rowBot + 8.0f * s;

        const bool sel = (row == selectedRow);
        const bool hov = hover.panel == InventoryUI::SlotHit::JOURNAL_ROW && hover.index == row;

        // The SELECTED row gets a filled plate, not just brighter text: on a page whose rows are
        // already four different colours, "slightly lighter" is not a selection cue.
        if (sel)      fillRect(r.listX, rowBot, r.listX + r.listW, rowBot + r.rowH,
                               Vec3{0.115f, 0.105f, 0.075f});
        else if (hov) fillRect(r.listX, rowBot, r.listX + r.listW, rowBot + r.rowH,
                               Vec3{0.070f, 0.070f, 0.088f});

        drawQuestMarker(r.listX + 12.0f * s, rowBot + r.rowH * 0.5f, 5.5f * s, st, stateColour(st));
    }

    flushHUD();   // tabs, divider, row plates and markers land NOW — before any glyph

    // Labels: the act tabs, then each quest row.
    for (u32 t = 0; t < InventoryUI::JOURNAL_TABS; t++) {
        const f32 x0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);
        const bool on  = (t == actTab);
        const bool hov = hover.panel == InventoryUI::SlotHit::JOURNAL_TAB && hover.index == t;
        const char* label = (t == 0) ? "ACT I" : "ACT II";
        const Vec3  col   = on  ? Vec3{1.0f, 0.90f, 0.50f}
                          : hov ? Vec3{0.85f, 0.85f, 0.90f}
                                : Vec3{0.52f, 0.52f, 0.58f};
        const f32 lw = FontSystem::textWidth(label, tx);
        FontSystem::drawText(sw, sh, x0 + (r.tabW - lw) * 0.5f, r.tabY + 7.0f * s, label, col, tx);
    }

    for (u8 row = 0; row < rowCount; row++) {
        const u8 q = rowQuest[row];
        const Quest::State st = static_cast<Quest::State>(prog.state[q]);
        const f32 y = r.listTopY - r.rowH * (static_cast<f32>(row) + 1.0f) + 8.0f * s;
        // A locked quest shows that it EXISTS without naming it. The player can see the act has
        // more to give; they just have not found it yet — the minimap's "dim while unexplored"
        // rule, applied to text.
        const char* label = (st == Quest::State::LOCKED) ? "? ? ?" : Quest::QUESTS[q].name;
        FontSystem::drawText(sw, sh, r.listX + 26.0f * s, y, label, stateColour(st), tx);
    }

    // Act progress under the list — D2's quest log tells you how much of the act is behind you,
    // and it is the one number the list itself cannot show at a glance.
    {
        char sum[48];
        std::snprintf(sum, sizeof(sum), "%u of %u complete",
                      static_cast<u32>(doneCount), static_cast<u32>(rowCount));
        FontSystem::drawText(sw, sh, r.listX + 4.0f * s,
                             r.listTopY - r.rowH * static_cast<f32>(rowCount) - 20.0f * s,
                             sum, Vec3{0.50f, 0.52f, 0.60f}, 0.9f * s);
    }

    if (selectedRow >= rowCount) return;   // empty act, or a stale cursor after a tab flip

    // --- Reading pane: title, giver, narration, objectives --------------------------------------
    const u8 q = rowQuest[selectedRow];
    const Quest::QuestDef& qd = Quest::QUESTS[q];
    const Quest::State st = static_cast<Quest::State>(prog.state[q]);
    const f32 lineH = 17.0f * s;
    f32 y = r.detailTopY - lineH;

    if (st == Quest::State::LOCKED) {
        FontSystem::drawText(sw, sh, r.detailX, y, "Not yet known.",
                             Vec3{0.45f, 0.45f, 0.5f}, tx);
        return;
    }

    FontSystem::drawText(sw, sh, r.detailX, y, qd.name, stateColour(st), 1.25f * s);
    y -= lineH * 0.55f;
    pushLine(r.detailX, y, r.detailX + r.detailW * 0.75f, y, Vec3{0.34f, 0.30f, 0.20f});
    flushHUD();
    y -= lineH * 0.95f;

    // giverIdx is authored data; an out-of-range one would read past GIVERS[] rather than merely
    // showing the wrong name, so it is bounded here and not trusted.
    if (qd.giverIdx < Quest::GIVER_COUNT) {
        FontSystem::drawText(sw, sh, r.detailX, y, Quest::GIVERS[qd.giverIdx].name,
                             Vec3{0.62f, 0.68f, 0.80f}, 0.95f * s);
        y -= lineH * 1.5f;
    }

    y = drawWrapped(sw, sh, r.detailX, y, r.detailW, qd.narration,
                    Vec3{0.80f, 0.80f, 0.84f}, tx, lineH);
    y -= lineH * 1.1f;

    FontSystem::drawText(sw, sh, r.detailX, y, "OBJECTIVES", Vec3{0.85f, 0.74f, 0.40f}, 0.9f * s);
    y -= lineH * 1.25f;

    for (u32 o = 0; o < qd.objectiveCount && o < Quest::MAX_OBJ; o++) {
        const Quest::ObjectiveDef& od = qd.objectives[o];
        // A COMPLETE quest ticks every objective, whatever `obj` holds. That is not cosmetic
        // licence — it is the only honest reading of the two states the engine can produce:
        //   - a v6 hero MIGRATES in as COMPLETE with `obj` zeroed, because a completion mask
        //     carries no objective detail to restore (Quest::migrateFromMask);
        //   - and TALK is excluded from the completion test on purpose, so a quest finished by
        //     its deed alone legitimately has an untouched conversation objective.
        // In both cases the quest IS done, so drawing "QUEST COMPLETE" above a column of empty
        // boxes states the opposite of the truth — which is exactly what it did.
        const bool done = (st == Quest::State::COMPLETE) ||
                          Quest::objectiveDone(prog, q, static_cast<u8>(o));

        char row[128];
        if (od.trigger == Quest::Trigger::CLEAR_ZONE && q == liveQuestIdx && !done) {
            // The live pool count — the same number the completion poll reads, so what the player
            // sees and what finishes the quest can never be two different numbers.
            //
            // Clamped because a breeder (Hot Reloader summons Null Pointers) can push the live
            // count ABOVE the count recorded at spawn, and an unsigned wrap would print 65534/4.
            const u32 killed = (liveRemaining >= liveTotal)
                             ? 0u : static_cast<u32>(liveTotal - liveRemaining);
            std::snprintf(row, sizeof(row), "%s  %u/%u", od.text,
                          killed, static_cast<u32>(liveTotal));
        } else if (od.required > 1) {
            std::snprintf(row, sizeof(row), "%s  %u/%u", od.text,
                          static_cast<u32>(Quest::objectiveProgress(prog, q, static_cast<u8>(o))),
                          static_cast<u32>(od.required));
        } else {
            std::snprintf(row, sizeof(row), "%s", od.text);
        }

        // The tick is drawn, not typed: "[x]" and "[ ]" differ by one glyph in the middle of a
        // fixed-width bracket pair, which at HUD scale is nearly invisible. A filled vs hollow box
        // reads instantly and matches the quest markers in the list beside it.
        const Vec3 col = done ? Vec3{0.55f, 0.85f, 0.55f} : Vec3{0.80f, 0.80f, 0.84f};
        const f32  bx = r.detailX + 2.0f * s, by = y + 1.0f * s, bs = 8.0f * s;
        if (done) fillRect(bx, by, bx + bs, by + bs, col);
        else      pushQuad(bx, by, bx + bs, by + bs, Vec3{0.45f, 0.45f, 0.50f});
        flushHUD();

        FontSystem::drawText(sw, sh, bx + bs + 8.0f * s, y, row, col, tx);
        y -= lineH * 1.15f;
    }

    if (st == Quest::State::COMPLETE) {
        y -= lineH * 0.4f;
        FontSystem::drawText(sw, sh, r.detailX, y, "QUEST COMPLETE",
                             Vec3{1.0f, 0.85f, 0.35f}, tx);
    }

    // Footer: the keyboard's own controls, named EXACTLY. The first version said "Left/Right
    // change act" while the nav read WASD only, so the arrow keys it pointed at did nothing —
    // a label that sends the player at a key which is not wired is worse than no label.
    FontSystem::drawText(sw, sh, r.detailX, m.contentY + 6.0f * s,
                         "W/S select quest    A/D change act    W at the top for the page tabs",
                         Vec3{0.44f, 0.44f, 0.52f}, 0.9f * s);
}
