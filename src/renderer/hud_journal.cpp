// hud_journal.cpp — the quest Journal, drawn as a panel of the inventory screen.
//
// The Journal replaced a chat line as the game's quest record. That matters for one concrete
// reason beyond legibility: Engine::addChatMessage formats into a fixed buffer, so nine of the ten
// quest blurbs were being truncated mid-sentence and several lost the part that says what to DO.
// Text here is word-WRAPPED against the panel width instead, so a narration can be any length.
//
// Geometry comes entirely from InventoryUI::journalLayout — this file never re-derives a rect.
#include "renderer/hud.h"
#include "renderer/hud_internal.h"   // pushLine/flushHUD — the opaque backing
#include "renderer/font.h"
#include "game/inventory_ui.h"
#include "game/quest_def.h"
#include "game/quest_state.h"

#include <cstdio>   // snprintf for the objective rows

namespace {

// Colour by state. Shape carries WHAT, colour carries WHICH — the same rule the overworld's
// minimap icons follow.
Vec3 stateColour(Quest::State s) {
    switch (s) {
        case Quest::State::COMPLETE: return Vec3{1.00f, 0.85f, 0.35f};   // gold
        case Quest::State::ACTIVE:   return Vec3{0.95f, 0.95f, 0.95f};   // white
        case Quest::State::OFFERED:  return Vec3{0.70f, 0.76f, 0.88f};   // grey-blue
        default:                     return Vec3{0.38f, 0.38f, 0.42f};   // dim: not yet known
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

void HUD::drawJournalPanel(u32 sw, u32 sh, const Quest::Progress& prog,
                           u8 selectedRow, u8 actTab,
                           u8 liveQuestIdx, u16 liveRemaining, u16 liveTotal,
                           s32 mouseX, s32 mouseY) {
    const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);
    const InventoryUI::SlotHit hover  = InventoryUI::hitTestJournal(sw, sh, mouseX, mouseY);
    const f32 s  = r.uiScale;
    const f32 tx = 1.0f * s;   // text scale

    // Opaque backing, drawn first — the same thing drawStashPanel does, and for the same reason:
    // the equipment column and the BACKPACK GRID both sit underneath this area. Without it the
    // narration is drawn straight over item icons and is unreadable on any bag that is not empty
    // (which the first screenshots of this panel could not show, because the test hero's bag was).
    //
    // Extents come from JournalRects alone — this file re-derives no geometry, which is the whole
    // point of the layout split. The LIST column is the deeper of the two, so its full row span is
    // what sets the bottom edge; the detail pane's worst case (title + narration + giver +
    // objectives) is shorter than that.
    {
        const f32 leftX  = r.listX;
        const f32 rightX = r.detailX + r.detailW;
        const f32 topY   = r.tabY + r.tabH;
        const f32 botY   = r.listTopY - r.rowH * static_cast<f32>(InventoryUI::JOURNAL_ROWS);
        const f32 pad    = 10.0f * s;
        // Cold ink rather than the stash's warm {0.07,0.06,0.04}: at a glance the two full-width
        // panels must not read as the same screen. Dark, because every text colour on this panel
        // (white / gold / green) is chosen to sit on a dark ground.
        const Vec3 bg = {0.05f, 0.052f, 0.072f};
        for (f32 fy = botY - pad; fy < topY + pad; fy += 1.0f)
            pushLine(leftX - pad, fy, rightX + pad, fy, bg);
        flushHUD();
    }

    // --- Act tabs ---
    for (u32 t = 0; t < InventoryUI::JOURNAL_TABS; t++) {
        const f32 x0  = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);
        const bool on = (t == actTab);
        const bool hov = hover.panel == InventoryUI::SlotHit::JOURNAL_TAB && hover.index == t;
        const Vec3 col = on  ? Vec3{1.0f, 0.9f, 0.5f}
                        : hov ? Vec3{0.85f, 0.85f, 0.9f}
                              : Vec3{0.55f, 0.55f, 0.6f};
        FontSystem::drawText(sw, sh, x0 + 8.0f * s, r.tabY + 6.0f * s,
                             t == 0 ? "ACT I" : "ACT II", col, tx);
    }

    // --- Quest list for the visible act ---
    // The rows are the act's quests IN TABLE ORDER, which is the road's walking order.
    u8 rowQuest[InventoryUI::JOURNAL_ROWS];
    u8 rowCount = 0;
    for (u32 i = 0; i < Quest::COUNT && rowCount < InventoryUI::JOURNAL_ROWS; i++) {
        if (Quest::actOf(Quest::QUESTS[i].zoneFloor) != actTab + 1) continue;
        rowQuest[rowCount++] = static_cast<u8>(i);
    }

    for (u8 row = 0; row < rowCount; row++) {
        const u8 q = rowQuest[row];
        const Quest::State st = static_cast<Quest::State>(prog.state[q]);
        // listTopY is row 0's TOP edge and rows DESCEND, so row i's BOTTOM edge is
        // listTopY - rowH*(i+1) — exactly what hitTestJournal's floor(dy/rowH) inverts. Placing the
        // baseline off by one rowH here is the draw/hit-test drift the layout split exists to stop.
        const f32 y = r.listTopY - r.rowH * (static_cast<f32>(row) + 1.0f) + 5.0f * s;

        const bool sel = (row == selectedRow);
        const bool hov = hover.panel == InventoryUI::SlotHit::JOURNAL_ROW && hover.index == row;
        Vec3 col = stateColour(st);
        if (sel || hov) col = Vec3{col.x + 0.15f, col.y + 0.15f, col.z + 0.15f};

        // A locked quest shows that it EXISTS without naming it. The player can see the act has
        // more to give; they just have not found it yet — the minimap's "dim while unexplored"
        // rule, applied to text.
        const char* label = (st == Quest::State::LOCKED) ? "? ? ?" : Quest::QUESTS[q].name;
        FontSystem::drawText(sw, sh, r.listX + (sel ? 10.0f * s : 4.0f * s), y, label, col, tx);
    }

    if (selectedRow >= rowCount) return;   // empty act, or a stale cursor after a tab flip

    // --- Detail pane: title, narration, objectives ---
    const u8 q = rowQuest[selectedRow];
    const Quest::QuestDef& qd = Quest::QUESTS[q];
    const Quest::State st = static_cast<Quest::State>(prog.state[q]);
    const f32 lineH = 16.0f * s;
    f32 y = r.detailTopY - lineH;

    if (st == Quest::State::LOCKED) {
        FontSystem::drawText(sw, sh, r.detailX, y, "Not yet known.",
                             Vec3{0.45f, 0.45f, 0.5f}, tx);
        return;
    }

    FontSystem::drawText(sw, sh, r.detailX, y, qd.name, stateColour(st), tx);
    y -= lineH * 1.6f;

    y = drawWrapped(sw, sh, r.detailX, y, r.detailW, qd.narration,
                    Vec3{0.78f, 0.78f, 0.82f}, tx, lineH);
    y -= lineH * 0.6f;

    // giverIdx is authored data; an out-of-range one would read past GIVERS[] rather than merely
    // showing the wrong name, so it is bounded here and not trusted.
    if (qd.giverIdx < Quest::GIVER_COUNT) {
        FontSystem::drawText(sw, sh, r.detailX, y, Quest::GIVERS[qd.giverIdx].name,
                             Vec3{0.55f, 0.60f, 0.70f}, tx * 0.9f);
    }
    y -= lineH * 1.4f;

    for (u32 o = 0; o < qd.objectiveCount && o < Quest::MAX_OBJ; o++) {
        const Quest::ObjectiveDef& od = qd.objectives[o];
        const bool done = Quest::objectiveDone(prog, q, static_cast<u8>(o));

        char row[128];
        if (od.trigger == Quest::Trigger::CLEAR_ZONE && q == liveQuestIdx && !done) {
            // The live pool count — the same number the completion poll reads, so what the player
            // sees and what finishes the quest can never be two different numbers.
            //
            // Clamped because a breeder (Hot Reloader summons Null Pointers) can push the live
            // count ABOVE the count recorded at spawn, and an unsigned wrap would print 65534/4.
            const u32 killed = (liveRemaining >= liveTotal)
                             ? 0u : static_cast<u32>(liveTotal - liveRemaining);
            std::snprintf(row, sizeof(row), "%s %s  %u/%u",
                          done ? "[x]" : "[ ]", od.text,
                          killed, static_cast<u32>(liveTotal));
        } else if (od.required > 1) {
            std::snprintf(row, sizeof(row), "%s %s  %u/%u",
                          done ? "[x]" : "[ ]", od.text,
                          static_cast<u32>(Quest::objectiveProgress(prog, q, static_cast<u8>(o))),
                          static_cast<u32>(od.required));
        } else {
            std::snprintf(row, sizeof(row), "%s %s", done ? "[x]" : "[ ]", od.text);
        }

        FontSystem::drawText(sw, sh, r.detailX, y, row,
                             done ? Vec3{0.55f, 0.85f, 0.55f} : Vec3{0.80f, 0.80f, 0.84f}, tx);
        y -= lineH;
    }
}
