---
name: changelog
description: Use when writing or updating CHANGELOG.md for DungeonEngine — collect every commit since the last v* tag, translate them into player-facing entries, and flag compatibility breaks (PROTOCOL_VERSION / SAVE_VERSION / controls.json). Trigger for "write a changelog", "changelog since the last tag", "what changed this release", "release notes".
---

# Write the changelog

Turn the commits since the last release tag into `CHANGELOG.md` entries a **player** can read.
Releases are tag-driven (`tools/release.sh` → CI publishes on a `v*` tag, see `docs/DEPLOYMENT.md`),
so the last tag is always the right cut point.

## Step 1 — Get the range

```bash
git describe --tags --abbrev=0            # the last release, e.g. v1.5.1
git log --oneline <lastTag>..HEAD         # everything since
git log <lastTag>..HEAD --stat            # when a subject is too terse to classify
```

If `git describe` finds nothing the repo has no tags yet — use the root commit and say so in the file.

## Step 2 — Classify, don't transcribe

Commit subjects are written for engineers; changelog entries are written for players. A commit that
says `fix(collision): entity obstacles block by shared space, not by XZ column` becomes
*"You couldn't walk on four-story floors."*

- **Added** — content or abilities that did not exist (a level style, an item, a skill, a mode).
- **Changed** — existing behaviour or balance that now behaves differently.
- **Fixed** — a bug the player could have hit. Describe the *symptom they'd have noticed*, not the
  mechanism. Lead with what was broken.
- **Omit** — pure refactors, test-only commits, doc syncs, and CI changes. They do not belong in a
  player-facing file. (`test:`/`docs:`/`chore:` prefixes are usually the tell, but read the diff:
  a "test" commit that also changed a constant is a Changed entry.)

Group the run's headline feature into a short lead paragraph before the sections — a reader should
learn what this release *is* in one sentence.

## Step 3 — Flag compatibility breaks LOUDLY

These strand players who don't update together, so they go in a blockquote at the top of the version,
never buried in a bullet:

| Constant | Where | Consequence to state |
|---|---|---|
| `PROTOCOL_VERSION` | `src/net/net.h` | Old clients **cannot join** — everyone must update together |
| `SAVE_VERSION` | `engine_persist.cpp` | Save-format change; say whether old saves still load |
| `BINDINGS_REV` | `controls.json` handling | Some key bindings reset to new defaults |

Check them explicitly rather than trusting commit subjects:

```bash
git diff <lastTag>..HEAD -- src/net/net.h | grep -i protocol_version
git diff <lastTag>..HEAD -- src/engine/engine_persist.cpp | grep -i save_version
```

Say nothing if they didn't move — and if the protocol moved but saves didn't, say saves are
unaffected, because that is the first thing a player worries about.

## Step 4 — Write it

Keep an `## [Unreleased]` section at the top while work is in flight; on release, rename it to
`## [X.Y.Z] — YYYY-MM-DD` and open a fresh `[Unreleased]`. Newest version first.

**Every entry must trace to a commit in the range.** Do not invent, do not soften, and do not list a
fix for something that was never broken in a *released* build — a bug introduced and fixed inside the
same unreleased range never reached a player, so it does not belong in Fixed (it silently becomes
part of the feature that shipped). This is the most common way a changelog lies.

## Step 5 — Verify before claiming done

```bash
git log <lastTag>..HEAD --oneline | wc -l     # commits in range
```

Walk the list and confirm each commit is either represented in an entry or deliberately omitted per
Step 2. State the count both ways ("37 commits → 14 entries, 23 omitted as internal") so the
omissions are a decision on record rather than an oversight.

## Gotchas

- **A branch that was never merged is not in the range.** `git log <tag>..HEAD` covers the CURRENT
  branch only. If work lives on other branches, they are invisible here — check `git branch -a` and
  say which branches the changelog covers.
- **The last tag may not be an ancestor of HEAD** (tag cut on another branch). `git merge-base --is-ancestor <tag> HEAD`
  tells you; if it is not, the range is misleading and you need an explicit merge-base.
- **Version strings live in three places** and CI keys off the tag, not the files — `tools/release.sh`
  re-syncs `src/CMakeLists.txt` and `.github/workflows/build.yml`. The changelog is not one of them,
  so bumping a version by hand does not touch this file.
- CI already publishes GitHub's auto-generated notes (`generate_release_notes: true` in
  `.github/workflows/build.yml`); those are raw commit subjects. `CHANGELOG.md` is the curated
  version and the two are expected to differ.
