#!/usr/bin/env python3
"""Generate the player's manual PDF for "Curse of the Dungeon Engine" (Steam manual upload).

A themed, branded manual: a cover built from the library capsule art, then content pages on a
parchment background with gold/crimson headings. All facts are pulled from the game's own configs
and code (class table from engine_init.cpp kClassDefs, controls from platform/input.cpp, bosses
from bosses.json, etc.) — see tools/gen_manual_SOURCES note below.

Output: store/manual.pdf

Usage:
    python3 tools/gen_manual.py
"""

import os
import sys

try:
    from reportlab.lib.pagesizes import letter
    from reportlab.lib.units import inch
    from reportlab.lib import colors
    from reportlab.lib.styles import ParagraphStyle
    from reportlab.lib.enums import TA_CENTER, TA_LEFT
    from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle,
                                    PageBreak, Image as RLImage)
except ImportError:
    sys.exit("reportlab is required: pip install reportlab")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(ROOT, "store")
STEAM = os.path.join(STORE, "steam")
ASSETS = os.path.join(ROOT, "assets")

PAGE_W, PAGE_H = letter

# Brand palette
GOLD    = colors.Color(0.86, 0.70, 0.24)
GOLD_DK = colors.Color(0.55, 0.40, 0.12)
RED     = colors.Color(0.74, 0.16, 0.12)
INK     = colors.Color(0.13, 0.11, 0.09)
CREAM   = colors.Color(0.96, 0.935, 0.875)
CREAM2  = colors.Color(0.91, 0.875, 0.80)
DARK    = colors.Color(0.078, 0.071, 0.094)
LINE    = colors.Color(0.62, 0.52, 0.36)

# --- paragraph styles -------------------------------------------------------------------------
H1 = ParagraphStyle("H1", fontName="Helvetica-Bold", fontSize=19, textColor=RED, spaceBefore=2,
                    spaceAfter=9, leading=22)
H2 = ParagraphStyle("H2", fontName="Helvetica-Bold", fontSize=12.5, textColor=GOLD_DK,
                    spaceBefore=9, spaceAfter=4, leading=15)
BODY = ParagraphStyle("Body", fontName="Helvetica", fontSize=10.3, textColor=INK, leading=15,
                      spaceAfter=6, alignment=TA_LEFT)
LEAD = ParagraphStyle("Lead", parent=BODY, fontSize=11.5, leading=17, textColor=colors.Color(0.2, 0.12, 0.08))
CELL = ParagraphStyle("Cell", fontName="Helvetica", fontSize=8.6, textColor=INK, leading=10.6)
CELLB = ParagraphStyle("CellB", parent=CELL, fontName="Helvetica-Bold")


def P(t, s=BODY):
    return Paragraph(t, s)


def bullets(items):
    return [P("&bull;&nbsp;&nbsp;" + it) for it in items]


# --- page backgrounds -------------------------------------------------------------------------
def cover_page(canvas, doc):
    canvas.saveState()
    canvas.setFillColor(DARK)
    canvas.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    cap = os.path.join(STEAM, "library_capsule.png")          # 600x900 portrait: logo + key art
    if os.path.exists(cap):
        h = PAGE_H
        w = h * (600.0 / 900.0)
        canvas.drawImage(cap, (PAGE_W - w) / 2.0, 0, w, h, mask=None)
    # bottom banner
    canvas.setFillColor(colors.Color(0, 0, 0, 0.66))
    canvas.rect(0, 0, PAGE_W, 78, fill=1, stroke=0)
    canvas.setFillColor(GOLD)
    canvas.setFont("Helvetica-Bold", 22)
    canvas.drawCentredString(PAGE_W / 2.0, 44, "OFFICIAL PLAYER'S MANUAL")
    canvas.setFillColor(colors.Color(0.8, 0.78, 0.7))
    canvas.setFont("Helvetica", 10)
    canvas.drawCentredString(PAGE_W / 2.0, 26, "Descend. Loot. Survive the curse.")
    canvas.restoreState()


def content_page(canvas, doc):
    canvas.saveState()
    canvas.setFillColor(CREAM)
    canvas.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    # header
    canvas.setFillColor(GOLD_DK)
    canvas.setFont("Helvetica-Bold", 8.5)
    canvas.drawString(0.85 * inch, PAGE_H - 0.62 * inch, "CURSE OF THE DUNGEON ENGINE")
    canvas.setFillColor(colors.Color(0.45, 0.38, 0.3))
    canvas.setFont("Helvetica", 8.5)
    canvas.drawRightString(PAGE_W - 0.85 * inch, PAGE_H - 0.62 * inch, "PLAYER'S MANUAL")
    canvas.setStrokeColor(LINE)
    canvas.setLineWidth(1)
    canvas.line(0.85 * inch, PAGE_H - 0.70 * inch, PAGE_W - 0.85 * inch, PAGE_H - 0.70 * inch)
    # footer
    canvas.line(0.85 * inch, 0.62 * inch, PAGE_W - 0.85 * inch, 0.62 * inch)
    canvas.setFillColor(colors.Color(0.45, 0.38, 0.3))
    canvas.drawCentredString(PAGE_W / 2.0, 0.45 * inch, str(doc.page))
    canvas.restoreState()


# --- table helpers ----------------------------------------------------------------------------
def styled_table(data, col_widths, header=True):
    t = Table(data, colWidths=col_widths, repeatRows=1 if header else 0)
    style = [
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 5), ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4), ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
        ("GRID", (0, 0), (-1, -1), 0.5, LINE),
        ("ROWBACKGROUNDS", (0, 1 if header else 0), (-1, -1), [colors.white, CREAM2]),
    ]
    if header:
        style += [("BACKGROUND", (0, 0), (-1, 0), GOLD),
                  ("TEXTCOLOR", (0, 0), (-1, 0), INK),
                  ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
                  ("FONTSIZE", (0, 0), (-1, 0), 8.8)]
    t.setStyle(TableStyle(style))
    return t


# --- content ----------------------------------------------------------------------------------
def story():
    s = [PageBreak()]   # page 1 is the cover (drawn by cover_page); content starts page 2

    # ---- THE DESCENT ----
    s += [P("The Descent", H1),
          P("Deep beneath the world grinds the <b>Dungeon Engine</b> &mdash; a cursed mechanism that "
            "forges an endless, ever-shifting labyrinth and fills it with the restless dead. Take up "
            "blade, bow, or spell, descend its fifty floors, and snuff out the curse at its black heart.",
            LEAD),
          P("Your Quest", H2),
          P("Fight downward through <b>50 procedurally generated floors</b>. Every fifth floor a boss "
            "guards the way down &mdash; defeat it to descend. Reach <b>Floor 50</b> and destroy the "
            "<b>Grim Reaper</b> to break the curse. Then prove yourself again on <b>Nightmare</b>, and "
            "finally <b>Hell</b>, where the dungeon bites back twice as hard.", BODY),
          P("The Loop", H2)]
    s += bullets([
        "<b>Explore</b> the floor and cut down everything that moves.",
        "<b>Loot</b> fallen foes and chests; grab health &amp; energy globes to top up.",
        "<b>Equip</b> stronger gear and roll better magic affixes as you find them.",
        "<b>Slay the floor boss</b> guarding the stairs, then <b>descend</b>.",
        "Your hero, gear, and skills carry forward between floors and runs.",
    ])
    s += [PageBreak()]

    # ---- CONTROLS ----
    s += [P("Controls", H1),
          P("Defaults shown. Keyboard &amp; mouse and a gamepad are both fully supported; rebind any "
            "action in <b>Options &rarr; Controls</b>.", BODY)]
    ctrl = [["Action", "Keyboard / Mouse", "Gamepad"]]
    for a, k, g in [
        ("Move", "W A S D", "Left Stick"),
        ("Look / Aim", "Mouse", "Right Stick"),
        ("Attack / Fire", "Left Mouse", "Right Trigger"),
        ("Class Skill", "Right Mouse", "Right Bumper"),
        ("Block / Guard", "Left Ctrl", "Left Trigger"),
        ("Dodge Roll", "Left Shift", "Right Stick (click)"),
        ("Jump", "Space", "A"),
        ("Skills 1 - 4", "1  2  3  4", "D-pad U / R / D / L"),
        ("Boot Skill", "F", "LB + A"),
        ("Helmet Skill", "G", "LB + B"),
        ("Health Potion", "Q", "B"),
        ("Pick Up Item", "E", "X"),
        ("Reload", "R", "Y"),
        ("Target Lock (unused)", "Middle Mouse", "—"),
        ("Inventory", "Tab", "Start"),
        ("Pause / Menu", "Esc", "Back"),
    ]:
        ctrl.append([P(a, CELLB), P(k, CELL), P(g, CELL)])
    s += [styled_table(ctrl, [1.9 * inch, 2.4 * inch, 2.2 * inch]), PageBreak()]

    # ---- CLASSES ----
    s += [P("Heroes", H1),
          P("Choose one of <b>nine</b> classes. Each starts with its own weapon and four signature "
            "skills that unlock as you descend (Floors 1, 10, 20, 30) and grow stronger at Floors 5, "
            "20, 30 and 40.", BODY)]
    cls = [["Class", "HP", "Spd", "Energy", "Starts With", "Signature Skills"]]
    for name, hp, spd, en, wpn, sk, desc in [
        ("Warrior", "150", "5.0", "80", "Iron Sword", "Thunderclap, War Cry, Whirlwind, Earthquake",
         "Heavy melee fighter with crowd control"),
        ("Ranger", "80", "6.5", "100", "Short Bow", "Volley, Piercing Shot, Barrage, Mark Prey",
         "Rapid-fire sharpshooter with piercing shots"),
        ("Sorcerer", "70", "5.5", "150", "Wand of Sparks", "Fireball, Frozen Orb, Chain Lightning, Meteor Strike",
         "Devastating elemental magic, fragile body"),
        ("Rogue", "85", "7.0", "100", "Rusty Dagger", "Fan of Knives, Shadow Step, Poison Cloud, Shadow Dance",
         "Hit-and-run assassin with stealth and backstabs"),
        ("Paladin", "130", "5.0", "90", "Iron Sword", "Holy Smite, Holy Bombardment, Holy Nova, Divine Judgment",
         "Wrathful holy warrior raining judgment"),
        ("Combat Engineer", "100", "5.5", "120", "Pistol", "Shock Bolt, Deploy Turret, Tesla Coil, Mech Overdrive",
         "Turrets, tesla coils, and gadgets"),
        ("Marksman", "75", "6.0", "100", "Revolver", "Aimed Shot, Explosive Round, Overcharged Magazine, Headshot",
         "Anti-materiel sniper with penetrating shots"),
        ("Tinkerer", "90", "5.5", "110", "Pistol", "Swarm Deploy, Overclock, Detonate Swarm, Swarm Queen",
         "Swarm overlord who overwhelms with drone armies"),
        ("Wanderer", "90", "6.5", "110", "Iron Sword", "Deflect, Exploit Weakness, Adrenaline Surge, Death's Dance",
         "Dodge-counter duelist; rolling replaces blocking"),
    ]:
        cls.append([P("<b>%s</b><br/><font size=7 color='#7a5a20'>%s</font>" % (name, desc), CELL),
                    P(hp, CELL), P(spd, CELL), P(en, CELL), P(wpn, CELL), P(sk, CELL)])
    s += [styled_table(cls, [1.65 * inch, 0.34 * inch, 0.36 * inch, 0.5 * inch, 0.95 * inch, 2.2 * inch]),
          PageBreak()]

    # ---- COMBAT ----
    s += [P("Combat &amp; Survival", H1),
          P("Weapons fall into three families &mdash; pick what suits your hero:", BODY)]
    s += bullets([
        "<b>Melee</b> (swords, daggers, axes, cleavers): swing in a short arc; high burst up close.",
        "<b>Firearms</b> (pistols, revolvers, SMGs, carbines): hitscan &mdash; they hit instantly at range "
        "and use ammo clips. <b>Reload (R)</b> when empty.",
        "<b>Projectiles &amp; Magic</b> (bows, crossbows, wands, staves, thrown knives, molotovs): launch "
        "shots that travel &mdash; lead your targets.",
    ])
    s += [P("Staying Alive", H2)]
    s += bullets([
        "<b>Dodge Roll (Left Shift / R3)</b> &mdash; a quick roll with brief invulnerability; the Wanderer "
        "turns dodging through attacks into counter-hits.",
        "<b>Block / Guard (Left Ctrl / LT)</b> &mdash; raise your guard to soak frontal blows.",
        "<b>Health Potion (Q)</b> &mdash; a big instant heal on a short cooldown. Use it before you're cornered.",
        "<b>Globes</b> &mdash; slain enemies drop health and energy globes that top you up automatically.",
        "<b>Critical Hits</b> &mdash; every weapon has a crit chance and multiplier; better gear crits harder.",
    ])
    s += [PageBreak()]

    # ---- LOOT & SKILLS ----
    s += [P("Loot, Gear &amp; Skills", H1),
          P("Loot is the heart of the dungeon. Items drop in four rarities &mdash; "
            "<font color='#8a8a8a'><b>Common</b></font>, <font color='#3a6fb0'><b>Magic</b></font>, "
            "<font color='#b89a20'><b>Rare</b></font> and <font color='#c0501e'><b>Legendary</b></font> "
            "&mdash; with higher rarities rolling more magic <b>affixes</b> (up to four) and higher values.", BODY),
          P("Equipment", H2),
          P("Six gear slots: <b>Weapon, Offhand (shield), Helmet, Armor, Boots</b> and <b>Ring</b>. Your "
            "backpack holds <b>24</b> items &mdash; open the <b>Inventory (Tab)</b> to compare and equip.", BODY),
          P("Affixes (examples)", H2)]
    aff = [["Affix", "Effect"], ]
    for n, e in [("of Striking", "+ flat weapon damage"), ("of Fury", "+ % damage"),
                 ("of Vitality / of the Titan", "+ flat / % maximum health"),
                 ("of Haste", "- skill cooldowns"), ("of the Wind", "+ movement speed"),
                 ("of Leeching / of the Vampire", "heal on hit / steal a % of damage dealt"),
                 ("of Quick Hands / of Capacity", "faster reload / bigger ammo clip"),
                 ("of Arcana", "+ maximum energy")]:
        aff.append([P(n, CELLB), P(e, CELL)])
    s += [styled_table(aff, [2.4 * inch, 4.1 * inch]),
          P("Skills", H2),
          P("Your four <b>class skills</b> sit on keys <b>1&ndash;4</b> and cost energy (or, for some, health). "
            "On top of those, <b>Legendary</b> gear can grant extra abilities: legendary <b>Boots</b> add a "
            "skill on <b>F</b>, legendary <b>Helmets</b> add one on <b>G</b>, and legendary weapons &amp; rings "
            "grant powerful passives or on-hit effects (lifesteal, thorns, berserker rage, void zones, seeking "
            "shadow bolts, and more). Mix and match to build your own engine of destruction.", BODY),
          PageBreak()]

    # ---- BESTIARY & BOSSES ----
    s += [P("Bestiary &amp; Bosses", H1),
          P("The dungeon shifts character as you descend, each ten-floor tier introducing deadlier foes:", BODY)]
    s += bullets([
        "<b>Floors 1&ndash;10 &mdash; Dungeon:</b> shambling corpses, bone archers, carrion bats, spiders, gargoyles.",
        "<b>Floors 11&ndash;20 &mdash; Catacombs:</b> poison skeletons &amp; spiders, bone mages.",
        "<b>Floors 21&ndash;30 &mdash; Caverns:</b> brood-spawning spiders and cave horrors.",
        "<b>Floors 31&ndash;40 &mdash; Hell's Gate:</b> hellhounds, demon flyers, infernal casters.",
        "<b>Floors 41&ndash;50 &mdash; The Void:</b> shades, void demons, teleporting spectral casters.",
    ])
    s += [P("Every fifth floor, a boss bars the stairs. <b>Major</b> bosses (Floors 10, 20, 30, 40, 50) are "
           "multi-phase brutes that guarantee Legendary loot.", BODY)]
    boss = [["Floor", "Boss", "Threat"]]
    for fl, nm, th in [
        ("5", "The Butcher", "Charging brute with a cleaver"),
        ("10", "Ygara, the Broodqueen ★", "Summons a poison brood under a toxic aura"),
        ("15", "Sethrak the Undying", "Caster-summoner, hurls chain lightning"),
        ("20", "Malachar, Warden of Tombs ★", "Teleporting frost mage, two phases"),
        ("25", "Ixara, Cavern Mother", "Spider queen that summons and heals"),
        ("30", "Korvath, the Siege Lord ★", "Shielded charger with an earthquake slam"),
        ("35", "Azhar, the Ashen Blade", "Fire duelist; burns over time"),
        ("40", "DiaBRO ★", "Berserker charger raining meteors"),
        ("45", "Nyx, Weaver of the Void", "Teleporting archmage of the void"),
        ("50", "Grim Reaper ★", "Final curse: summons, aura, and blood nova"),
    ]:
        boss.append([P(fl, CELLB), P(nm, CELL), P(th, CELL)])
    s += [styled_table(boss, [0.6 * inch, 2.5 * inch, 3.4 * inch]),
          P("<font size=8 color='#7a5a20'>★ = Major boss (guaranteed Legendary drop)</font>", CELL),
          PageBreak()]

    # ---- CO-OP, DIFFICULTY & TIPS ----
    s += [P("Co-op, Difficulty &amp; Saving", H1),
          P("Play With Friends", H2),
          P("Brave the dungeon with up to <b>four</b> players. <b>Host</b> a game and friends <b>Join</b> by "
            "address, or share one PC in <b>two-player split-screen</b> co-op &mdash; you can even mix the two "
            "(local + online together). Everyone explores the same seeded dungeon; downed heroes can respawn so "
            "the party fights on.", BODY),
          P("Difficulty", H2),
          P("Clear Floor 50 on <b>Normal</b> to unlock <b>Nightmare</b>, then <b>Hell</b> &mdash; the same fifty "
            "floors, far more lethal, with richer rewards. Enemies also grow ~10% stronger every floor you "
            "descend.", BODY),
          P("Your Hero Persists", H2),
          P("Each character has its own save, remembering its floor, difficulty, gear and inventory. There is no "
            "permadeath: fall in solo play and you return to your last save; fall in co-op and you respawn with "
            "the party.", BODY),
          P("Survival Tips", H2)]
    s += bullets([
        "Pick a class that fits how you like to fight &mdash; tanky Warrior/Paladin, glass-cannon Sorcerer, "
        "slippery Rogue/Wanderer, or gadget-and-swarm Engineer/Tinkerer.",
        "Always compare drops; a fresh Rare with the right affixes often beats an old Legendary.",
        "Hold some energy and your potion for the boss &mdash; major bosses change tactics at low health.",
        "Time your dodge roll through attacks for the invulnerability window.",
        "Hunt Legendary boots and helmets: the extra F / G skills define your build.",
    ])
    s += [Spacer(1, 16),
          P("<font color='#7a5a20'><b>Curse of the Dungeon Engine</b> &mdash; now go break it.</font>",
            ParagraphStyle("end", parent=BODY, alignment=TA_CENTER, fontSize=11))]
    return s


def main():
    out = os.path.join(STORE, "manual.pdf")
    os.makedirs(STORE, exist_ok=True)
    doc = SimpleDocTemplate(out, pagesize=letter,
                            leftMargin=0.85 * inch, rightMargin=0.85 * inch,
                            topMargin=0.95 * inch, bottomMargin=0.85 * inch,
                            title="Curse of the Dungeon Engine - Player's Manual",
                            author="Curse of the Dungeon Engine")
    doc.build(story(), onFirstPage=cover_page, onLaterPages=content_page)
    print("Wrote", out)


if __name__ == "__main__":
    main()
