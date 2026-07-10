// Steam platform module — Steamworks lifecycle (SteamAPI init/shutdown/callback pump) plus the relay
// networking + matchmaking helpers used by src/net/ and the menus (added in later phases). Everything
// is gated on USE_STEAM (set by external/CMakeLists.txt when external/steamworks/ is present and not
// Switch); without it every function is a no-op so the engine builds and runs unchanged. steam.h stays
// SDK-free, so this file is the ONLY place the Steamworks headers are included.
#include "platform/steam.h"
#include "core/log.h"

#ifdef USE_STEAM
#include "steam/steam_api.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/isteammatchmaking.h"
#include "steam/isteamfriends.h"
#include <cstdio>   // snprintf for the rich-presence connect string
#include <cstdlib>  // atoi — parse the host-published "players" lobby metadata for the browser

static bool s_available = false;

// CSteamID from our u64: the cSteamId(u64) constructor is ambiguous (Steam's uint64 is
// unsigned long long, our u64 is unsigned long), so build it via SetFromUint64.
static CSteamID cSteamId(u64 id) { CSteamID s; s.SetFromUint64(static_cast<uint64>(id)); return s; }

// --- Lobby / matchmaking (P2) --------------------------------------------------------------------
static u64 s_currentLobby = 0;
static Steam::OnLobbyEnteredFn s_onLobbyEntered = nullptr;
static Steam::OnLobbyCreatedFn s_onLobbyCreated = nullptr;
static Steam::OnLobbyListFn    s_onLobbyList    = nullptr;

// Owns the async CreateLobby / JoinLobby call-results and the invite/join-request callback. One
// instance, created in Steam::init.
class SteamLobbyMgr {
public:
    void create(bool friendsOnly, int maxMembers) {
        ELobbyType t = friendsOnly ? k_ELobbyTypeFriendsOnly : k_ELobbyTypePublic;
        m_createResult.Set(SteamMatchmaking()->CreateLobby(t, maxMembers), this, &SteamLobbyMgr::onCreated);
    }
    void join(u64 lobbyId) {
        m_enterResult.Set(SteamMatchmaking()->JoinLobby(cSteamId(lobbyId)), this, &SteamLobbyMgr::onEntered);
    }
    void requestList(const char* version) {
        SteamMatchmaking()->AddRequestLobbyListStringFilter("version", version, k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListFilterSlotsAvailable(1);   // >=1 open slot
        m_listResult.Set(SteamMatchmaking()->RequestLobbyList(), this, &SteamLobbyMgr::onList);
    }
    int listCount() const { return m_listCount; }
    u64 listEntry(int i) const { return (i >= 0 && i < m_listCount) ? m_list[i] : 0; }
private:
    static const int MAX_LIST = 32;
    u64 m_list[MAX_LIST] = {0};
    int m_listCount = 0;
    void onList(LobbyMatchList_t* r, bool ioFail) {
        m_listCount = 0;
        if (!ioFail) {
            int n = static_cast<int>(r->m_nLobbiesMatching);
            if (n > MAX_LIST) n = MAX_LIST;
            for (int i = 0; i < n; i++) m_list[i] = SteamMatchmaking()->GetLobbyByIndex(i).ConvertToUint64();
            m_listCount = n;
        }
        LOG_INFO("Steam: lobby list — %d matching", m_listCount);
        if (s_onLobbyList) s_onLobbyList(m_listCount);
    }
    CCallResult<SteamLobbyMgr, LobbyMatchList_t> m_listResult;
    void onCreated(LobbyCreated_t* r, bool ioFail) {
        bool ok = (!ioFail && r->m_eResult == k_EResultOK);
        s_currentLobby = ok ? r->m_ulSteamIDLobby : 0;
        if (ok) {
            // Rich-presence "connect" makes the friends-list "Join Game" work (warm + cold start).
            char cmd[64];
            std::snprintf(cmd, sizeof(cmd), "+connect_lobby %llu", (unsigned long long)s_currentLobby);
            SteamFriends()->SetRichPresence("connect", cmd);
            LOG_INFO("Steam: lobby created %llu", (unsigned long long)s_currentLobby);
        } else {
            LOG_WARN("Steam: CreateLobby failed");
        }
        if (s_onLobbyCreated) s_onLobbyCreated(s_currentLobby, ok);
    }
    void onEntered(LobbyEnter_t* r, bool ioFail) {
        if (ioFail || r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
            LOG_WARN("Steam: lobby enter failed");
            return;
        }
        s_currentLobby = r->m_ulSteamIDLobby;
        u64 owner = SteamMatchmaking()->GetLobbyOwner(cSteamId(s_currentLobby)).ConvertToUint64();
        LOG_INFO("Steam: entered lobby %llu (host %llu)",
                 (unsigned long long)s_currentLobby, (unsigned long long)owner);
        if (s_onLobbyEntered) s_onLobbyEntered(s_currentLobby, owner);
    }
    STEAM_CALLBACK(SteamLobbyMgr, onJoinRequested, GameLobbyJoinRequested_t);
    CCallResult<SteamLobbyMgr, LobbyCreated_t> m_createResult;
    CCallResult<SteamLobbyMgr, LobbyEnter_t>   m_enterResult;
};
void SteamLobbyMgr::onJoinRequested(GameLobbyJoinRequested_t* e) {
    // Friend invited us / clicked "Join Game" while we're running → join that lobby.
    join(e->m_steamIDLobby.ConvertToUint64());
}
static SteamLobbyMgr* s_lobbyMgr = nullptr;
#endif

namespace Steam {

bool init() {
#ifdef USE_STEAM
    // [diag] Granular markers (flushed per line by log.cpp) to pinpoint which Steam call crashes the
    // Windows build — the mingw↔MSVC C++ ABI makes virtual interface calls into steamclient64.dll fault.
    // The LAST "[diag]" line in DungeonEngine.log before the crash names the exact culprit call.
    LOG_INFO("Steam: [diag] calling SteamAPI_Init...");
    // Needs steam_appid.txt next to the binary (dev) or a Steam launch, and a running Steam client.
    if (!SteamAPI_Init()) {
        LOG_WARN("Steam: SteamAPI_Init failed (no Steam client, or bad/missing App ID) — Steam features off");
        return false;
    }
    LOG_INFO("Steam: [diag] SteamAPI_Init OK — calling SteamNetworkingUtils()->InitRelayNetworkAccess...");
    // Warm the Steam Datagram Relay early so the first P2P connect isn't stalled waiting for a route.
    if (SteamNetworkingUtils()) SteamNetworkingUtils()->InitRelayNetworkAccess();
    LOG_INFO("Steam: [diag] InitRelayNetworkAccess OK — creating SteamLobbyMgr (registers callback)...");
    s_available = true;
    s_lobbyMgr = new SteamLobbyMgr();   // registers the invite/join-request callback
    LOG_INFO("Steam: [diag] SteamLobbyMgr OK — calling SteamUser()->GetSteamID (localSteamId)...");
    LOG_INFO("Steam: initialized (SteamID %llu)", (unsigned long long)localSteamId());
    return true;
#else
    return false;
#endif
}

void shutdown() {
#ifdef USE_STEAM
    if (s_lobbyMgr) { delete s_lobbyMgr; s_lobbyMgr = nullptr; }
    if (s_available) { SteamAPI_Shutdown(); s_available = false; }
#endif
}

void runCallbacks() {
#ifdef USE_STEAM
    if (s_available) {
        // [diag] Mark the first dispatch — if the crash lands here (not in init), it's the auto-dispatch
        // callback path (STEAM_CALLBACK/CCallResult) rather than a synchronous interface call.
        static bool s_firstCb = true;
        if (s_firstCb) LOG_INFO("Steam: [diag] first SteamAPI_RunCallbacks...");
        SteamAPI_RunCallbacks();
        if (s_firstCb) { LOG_INFO("Steam: [diag] first SteamAPI_RunCallbacks OK"); s_firstCb = false; }
    }
#endif
}

bool isAvailable() {
#ifdef USE_STEAM
    return s_available;
#else
    return false;
#endif
}

u64 localSteamId() {
#ifdef USE_STEAM
    if (!s_available || !SteamUser()) return 0;
    return SteamUser()->GetSteamID().ConvertToUint64();
#else
    return 0;
#endif
}

// --- Lobby / matchmaking (P2) ---

void createLobby(bool friendsOnly, int maxMembers) {
#ifdef USE_STEAM
    if (s_available && s_lobbyMgr) s_lobbyMgr->create(friendsOnly, maxMembers);
#else
    (void)friendsOnly; (void)maxMembers;
#endif
}

void joinLobby(u64 lobbyId) {
#ifdef USE_STEAM
    if (s_available && s_lobbyMgr) s_lobbyMgr->join(lobbyId);
#else
    (void)lobbyId;
#endif
}

void leaveLobby() {
#ifdef USE_STEAM
    if (s_available && s_currentLobby) {
        SteamMatchmaking()->LeaveLobby(cSteamId(s_currentLobby));
        SteamFriends()->ClearRichPresence();
        s_currentLobby = 0;
    }
#endif
}

void closeLobby() {
#ifdef USE_STEAM
    // Mark non-joinable first (while we still own the lobby and can set flags) so a new joiner is
    // refused even if Steam transfers ownership to a remaining member instead of destroying the lobby.
    if (s_available && s_currentLobby) {
        SteamMatchmaking()->SetLobbyJoinable(cSteamId(s_currentLobby), false);
    }
    leaveLobby();   // removes it from discovery, clears our "Join Game" rich presence, zeroes s_currentLobby
#endif
}

u64 currentLobbyId() {
#ifdef USE_STEAM
    return s_currentLobby;
#else
    return 0;
#endif
}

u64 lobbyOwner(u64 lobbyId) {
#ifdef USE_STEAM
    if (s_available) return SteamMatchmaking()->GetLobbyOwner(cSteamId(lobbyId)).ConvertToUint64();
#else
    (void)lobbyId;
#endif
    return 0;
}

void setLobbyData(const char* key, const char* value) {
#ifdef USE_STEAM
    if (s_available && s_currentLobby) SteamMatchmaking()->SetLobbyData(cSteamId(s_currentLobby), key, value);
#else
    (void)key; (void)value;
#endif
}

void setLobbyJoinable(bool joinable) {
#ifdef USE_STEAM
    if (s_available && s_currentLobby) SteamMatchmaking()->SetLobbyJoinable(cSteamId(s_currentLobby), joinable);
#else
    (void)joinable;
#endif
}

void openInviteOverlay() {
#ifdef USE_STEAM
    if (s_available && s_currentLobby) SteamFriends()->ActivateGameOverlayInviteDialog(cSteamId(s_currentLobby));
#endif
}

void setOnLobbyEntered(OnLobbyEnteredFn fn) {
#ifdef USE_STEAM
    s_onLobbyEntered = fn;
#else
    (void)fn;
#endif
}

void setOnLobbyCreated(OnLobbyCreatedFn fn) {
#ifdef USE_STEAM
    s_onLobbyCreated = fn;
#else
    (void)fn;
#endif
}

// --- Public lobby browser / quickmatch (P3) ---

void requestLobbyList(const char* version) {
#ifdef USE_STEAM
    if (s_available && s_lobbyMgr) s_lobbyMgr->requestList(version);
#else
    (void)version;
#endif
}

int lobbyListCount() {
#ifdef USE_STEAM
    return s_lobbyMgr ? s_lobbyMgr->listCount() : 0;
#else
    return 0;
#endif
}

u64 lobbyListEntry(int i, char* nameBuf, int nameCap, int* memberCount, int* maxMembers) {
#ifdef USE_STEAM
    if (!s_available || !s_lobbyMgr) return 0;
    u64 id = s_lobbyMgr->listEntry(i);
    if (!id) return 0;
    CSteamID cid = cSteamId(id);
    if (nameBuf && nameCap > 0) {
        const char* nm = SteamMatchmaking()->GetLobbyData(cid, "name");
        std::snprintf(nameBuf, nameCap, "%s", (nm && nm[0]) ? nm : "Dungeon Engine");
    }
    if (memberCount) {
        // Prefer the host-published authoritative in-game roster ("players") over the Steam lobby member
        // count: joiners connect over the relay independently of lobby membership, so GetNumLobbyMembers
        // can lag or diverge from the real number of players in the game. Fall back to it if the host
        // hasn't published yet (older lobby / pre-creation-data race).
        const char* pl = SteamMatchmaking()->GetLobbyData(cid, "players");
        int published = (pl && pl[0]) ? std::atoi(pl) : 0;
        *memberCount = (published > 0) ? published : SteamMatchmaking()->GetNumLobbyMembers(cid);
    }
    if (maxMembers)  *maxMembers  = SteamMatchmaking()->GetLobbyMemberLimit(cid);
    return id;
#else
    (void)i; (void)nameBuf; (void)nameCap; (void)memberCount; (void)maxMembers; return 0;
#endif
}

void setOnLobbyList(OnLobbyListFn fn) {
#ifdef USE_STEAM
    s_onLobbyList = fn;
#else
    (void)fn;
#endif
}

}  // namespace Steam
