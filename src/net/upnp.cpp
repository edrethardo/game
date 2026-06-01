// UPnP IGD port-mapping wrapper. Real implementation calls miniupnpc; Switch
// builds compile to a stub via the absence of UPNP_AVAILABLE (set in
// src/CMakeLists.txt only for non-Switch targets). Keeping the platform
// boundary inside this file means Net::hostServer and the lobby UI have
// single-source call sites that don't need their own #ifdefs.

#include "net/upnp.h"
#include "core/log.h"

#include <cstdio>
#include <cstring>

#if defined(UPNP_AVAILABLE)
// Upstream miniupnpc's CMake exposes its include/ directory directly as the
// public include path — the headers are at include/miniupnpc.h (not under a
// miniupnpc/ subdirectory). Use bare names instead of the conventional
// <miniupnpc/...> style so the include lines match how the vendored copy
// presents them.
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>
#endif

namespace Upnp {

// File-scope state for the currently-held mapping. Used so removePortMapping()
// can tear down what tryAddPortMapping() installed without the caller juggling
// opaque handles. There's only ever one host per process, so a single set of
// statics is sufficient.
static char s_externalIp[64] = {0};
static char s_lastError [128] = {0};

#if defined(UPNP_AVAILABLE)

// State retained between tryAddPortMapping and removePortMapping so the
// matching UPNP_DeletePortMapping call hits the right router/service.
static bool          s_haveMapping = false;
// Sized to match miniupnpc's internal buffers (servicetype is char[128] in IGDdatas;
// controlURL paths can run long when the IGD's URL contains UUIDs/paths).
static char          s_servicetype[128]  = {0};   // copied from IGDdatas
static char          s_controlURL [256]  = {0};   // copied from UPNPUrls
static char          s_extPortStr [8]    = {0};   // "7777", etc.

static void setError(const char* msg, char errorMsg[128]) {
    std::snprintf(s_lastError, sizeof(s_lastError), "%s", msg);
    if (errorMsg) std::snprintf(errorMsg, 128, "%s", msg);
}

bool tryAddPortMapping(u16 port,
                       u32 discoveryTimeoutMs,
                       char externalIp[64],
                       char errorMsg[128]) {
    if (externalIp) externalIp[0] = '\0';
    if (errorMsg)   errorMsg[0]   = '\0';
    s_externalIp[0] = '\0';
    s_lastError [0] = '\0';

    int err = 0;
    UPNPDev* devlist = upnpDiscover(
        static_cast<int>(discoveryTimeoutMs),
        /*multicastif*/ nullptr,
        /*minissdpdsock*/ nullptr,
        /*localport*/ UPNP_LOCAL_PORT_ANY,
        /*ipv6*/ 0,
        /*ttl*/ 2,
        &err);
    if (!devlist) {
        // err == UPNPDISCOVER_SUCCESS is also possible when no devices respond;
        // either way the result is "no IGD on this network".
        setError("no IGD found on LAN", errorMsg);
        LOG_INFO("upnp: discovery returned no devices (err=%d)", err);
        return false;
    }

    UPNPUrls urls{};
    IGDdatas data{};
    char lanaddr[64] = {0};
    char wanaddr[64] = {0};
    int igdResult = UPNP_GetValidIGD(devlist, &urls, &data,
                                     lanaddr, sizeof(lanaddr),
                                     wanaddr, sizeof(wanaddr));
    freeUPNPDevlist(devlist);
    // Return codes per upstream header:
    //   0 = no IGD,  1 = valid connected IGD,  2 = connected but reserved IP,
    //   3 = found but reports disconnected,    4 = UPnP device that isn't an IGD.
    if (igdResult != 1 && igdResult != 2) {
        FreeUPNPUrls(&urls);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "no valid IGD (status=%d)", igdResult);
        setError(buf, errorMsg);
        LOG_INFO("upnp: %s", buf);
        return false;
    }

    char extIp[64] = {0};
    int ipErr = UPNP_GetExternalIPAddress(urls.controlURL,
                                          data.first.servicetype,
                                          extIp);
    if (ipErr != UPNPCOMMAND_SUCCESS || extIp[0] == '\0') {
        FreeUPNPUrls(&urls);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "GetExternalIPAddress failed: %s",
                      strupnperror(ipErr));
        setError(buf, errorMsg);
        return false;
    }

    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    int addErr = UPNP_AddPortMapping(
        urls.controlURL,
        data.first.servicetype,
        /*extPort*/  portStr,
        /*inPort*/   portStr,
        /*inClient*/ lanaddr,
        /*desc*/     "DungeonEngine",
        /*proto*/    "UDP",
        /*remoteHost*/ nullptr,
        // leaseDuration 0 = permanent until removed. Some routers force a 7-day
        // cap; either way removePortMapping() on shutdown cleans up.
        /*leaseDuration*/ "0");
    if (addErr != UPNPCOMMAND_SUCCESS) {
        FreeUPNPUrls(&urls);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "AddPortMapping failed: %s",
                      strupnperror(addErr));
        setError(buf, errorMsg);
        return false;
    }

    // Stash service + control URL for the eventual DeletePortMapping. urls.controlURL
    // points into miniupnpc-allocated memory that FreeUPNPUrls will reclaim, so
    // copy the strings out before freeing.
    std::snprintf(s_servicetype, sizeof(s_servicetype), "%s", data.first.servicetype);
    std::snprintf(s_controlURL,  sizeof(s_controlURL),  "%s", urls.controlURL);
    std::snprintf(s_extPortStr,  sizeof(s_extPortStr),  "%s", portStr);
    FreeUPNPUrls(&urls);

    std::snprintf(s_externalIp, sizeof(s_externalIp), "%s", extIp);
    if (externalIp) std::snprintf(externalIp, 64, "%s", extIp);
    s_haveMapping = true;
    LOG_INFO("upnp: mapped UDP %s -> %s:%s on router (external IP %s)",
             portStr, lanaddr, portStr, extIp);
    return true;
}

void removePortMapping() {
    if (!s_haveMapping) return;
    int delErr = UPNP_DeletePortMapping(s_controlURL, s_servicetype,
                                        s_extPortStr, "UDP", nullptr);
    if (delErr != UPNPCOMMAND_SUCCESS) {
        LOG_WARN("upnp: DeletePortMapping failed: %s (ignored — session ending)",
                 strupnperror(delErr));
    } else {
        LOG_INFO("upnp: removed UDP %s mapping", s_extPortStr);
    }
    s_haveMapping = false;
    s_externalIp[0] = '\0';
    s_controlURL [0] = '\0';
    s_servicetype[0] = '\0';
    s_extPortStr [0] = '\0';
}

#else // !UPNP_AVAILABLE — Switch stub. Same API, returns "unsupported".

bool tryAddPortMapping(u16 /*port*/,
                       u32 /*discoveryTimeoutMs*/,
                       char externalIp[64],
                       char errorMsg[128]) {
    static const char* kMsg = "UPnP unsupported on this platform";
    std::snprintf(s_lastError, sizeof(s_lastError), "%s", kMsg);
    if (externalIp) externalIp[0] = '\0';
    if (errorMsg)   std::snprintf(errorMsg, 128, "%s", kMsg);
    return false;
}

void removePortMapping() {
    // No-op — nothing was mapped on this platform.
}

#endif

const char* currentExternalIp() { return s_externalIp; }
const char* lastError()         { return s_lastError;  }

} // namespace Upnp
