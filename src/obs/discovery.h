/*
 * Network discovery: announces this OBS on the LAN over mDNS/DNS-SD, so CTD Live Studio (phone and PC) can list the OBS
 * machines on the network and connect to obs-websocket (and its "ctd" vendor requests) without typing an address.
 *
 *   service  _ctd-obs._tcp.local, instance = this machine's name, port = obs-websocket's port
 *   TXT      v=1, auth=1|0 (a password is needed), plugin=<version>, obs=<OBS version>
 *
 * Only while obs-websocket's server is on; the announcement follows its port, its password requirement and the LAN
 * address. The password itself is never announced. Windows 10 1809+ (dnsapi) and macOS (Bonjour); not Linux yet.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void discovery_start(void);
void discovery_stop(void);

/* On by default; the Tools menu switch (src/ui/discovery-menu.cpp) sets it. */
void discovery_set_enabled(bool enabled);
bool discovery_enabled(void);

#ifdef __cplusplus
}
#endif
