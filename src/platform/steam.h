#pragma once
// Steamworks integration surface (relay networking + matchmaking). Kept SDK-free (only core types)
// so the rest of the engine can call it without pulling the proprietary Steamworks headers — all the
// SDK contact lives in steam.cpp behind #ifdef USE_STEAM. When the SDK is absent (external/steamworks/
// not present) or on Switch, USE_STEAM is undefined and every function below is a no-op / returns
// false, so the game builds and runs exactly as before. See docs/steam_cloud.md and the netplay design.
#include "core/types.h"

namespace Steam {

// --- Lifecycle (P0) ---
// SteamAPI_Init + relay warm-up. Returns false (and the game runs Steam-less) if the SDK is absent,
// no Steam client is running, or the App ID is bad. Safe to call always.
bool init();
void shutdown();
// Pump Steam callbacks — MUST be called every frame, unconditionally (invites arrive at the menu).
void runCallbacks();
// True only after a successful init(): gates the Steam menu options / net transport selection.
bool isAvailable();
// The local user's SteamID (0 if unavailable).
u64  localSteamId();
// The local user's Steam display name. Returns "" when Steam is unavailable. The host publishes this
// as the lobby's "name" so the public browser can actually tell games apart (every lobby used to
// advertise the same hardcoded title). Pointer is owned by Steam — copy it if you need to keep it.
const char* localPersonaName();

// --- Matchmaking / lobbies (P2) ---
// A Steam lobby is just a rendezvous carrying the host's SteamID; the actual game traffic runs over
// the relay transport (Net::hostServerSteam / connectToSteamHost). All calls are safe no-ops when
// Steam isn't available.
void createLobby(bool friendsOnly, int maxMembers);   // async -> onLobbyCreated
void joinLobby(u64 lobbyId);                            // async -> onLobbyEntered
void leaveLobby();                                      // host leaving destroys it
// Host-only "close the lobby" (pause-menu action): stop advertising + refuse new joiners WITHOUT ending
// the session. Sets the lobby non-joinable (belt-and-suspenders in case ownership transfers to a remaining
// member) then leaves it (removes it from the browser / friends "Join Game" and clears our rich presence).
// Existing players stay connected — relay sessions (ISteamNetworkingMessages) are independent of lobby
// membership. After this, currentLobbyId() == 0, so the roster-publish path self-disables.
void closeLobby();
u64  currentLobbyId();                                  // 0 if not in a lobby
u64  lobbyOwner(u64 lobbyId);                           // owner SteamID = the host to connect to
void setLobbyData(const char* key, const char* value); // e.g. "version","name","floor","slots_free"
void setLobbyJoinable(bool joinable);                  // false when the game is full
void openInviteOverlay();                              // Steam overlay "Invite Friends" for the lobby

// --- Achievements ---
// Unlock by API name (must match an achievement defined for App 4819550 on the Steamworks
// partner site — see docs/DEPLOYMENT.md → Achievements). Safe to call repeatedly (skips the
// StoreStats round-trip if already unlocked) and safe without Steam (no-op in itch builds /
// when the client isn't running), so game code calls it unconditionally at the trigger site.
void unlockAchievement(const char* apiName);

// Callbacks (set once at startup). onLobbyCreated(lobbyId, ok): the HOST then Net::hostServerSteam +
// sets lobby data. onLobbyEntered(lobbyId, ownerSteamId): a JOINER then Net::connectToSteamHost(owner)
// — unless ownerSteamId == localSteamId() (our own lobby, ignore). Accepting an invite / friends-list
// "Join Game" is handled internally (GameLobbyJoinRequested -> joinLobby -> onLobbyEntered).
using OnLobbyEnteredFn = void(*)(u64 lobbyId, u64 ownerSteamId);
using OnLobbyCreatedFn = void(*)(u64 lobbyId, bool ok);
void setOnLobbyEntered(OnLobbyEnteredFn fn);
void setOnLobbyCreated(OnLobbyCreatedFn fn);

// --- Public lobby browser / quickmatch (P3) ---
// Request the public lobby list: matching `version`, >=1 open slot, and NOT private. Async -> onLobbyList.
void requestLobbyList(const char* version);
// Look up the single lobby publishing `code` (the host's 4-glyph share code) — the only way into a
// PRIVATE lobby for someone who isn't a Steam friend. Private lobbies are excluded from the browser
// above but remain searchable by code. Async -> onLobbyList (count 0 = no such game).
void requestLobbyListByCode(const char* version, const char* code);
int  lobbyListCount();
// Read entry i (0-based). Returns the lobby id (0 if out of range); fills name/memberCount/maxMembers.
u64  lobbyListEntry(int i, char* nameBuf, int nameCap, int* memberCount, int* maxMembers);
// Read one host-published metadata key off browsed entry i (e.g. "floor", "difficulty"). Writes ""
// when Steam/the entry/the key is unavailable, so callers can always print the buffer.
void lobbyListData(int i, const char* key, char* buf, int cap);
using OnLobbyListFn = void(*)(int count);
void setOnLobbyList(OnLobbyListFn fn);

}  // namespace Steam
