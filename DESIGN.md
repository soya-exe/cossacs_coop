# Cossacks: Back to War: Steam virtual LAN

> **Superseded in part (2026-10-27).** Ghidra verification (re/FINDINGS.md) showed the native UDP
> transport has no LAN broadcast discovery: it unicasts to a host IP that an internet lobby
> supplies. The built hook therefore implements IChat's `Process_GSC_ChatWindow` as a Steam-lobby
> stand-in and unlocks the "GSC-Game.Net" connection button (patch at 0x55264e). See
> src/hook/README.md for the design as built. The sections below are the original plan.

Goal: two or more friends play Cossacks: Back to War multiplayer (skirmish, together vs AI or
each other) over Steam: invite through Steam, traffic through Valve's relay, no port forwarding,
no IP addresses. The game's own lockstep netcode stays unchanged; we only replace the wire.

Game: Steam app 4850, buildid 2684, `bin/dmcr.exe` (PE32 x86, v1.35, 2011-01-11),
SHA-256 `3923b0ff3ae8da0584f29ae2bbf2e2deb030e57df11d37fb6fb55fa5ceb4acd6`.
Install: `<Steam library>/steamapps/common/Cossacks Back to War/`.
Runs under Proton on Arch Linux. Toolchain: `i686-w64-mingw32-gcc` (mingw-w64-gcc 16), Ghidra
12.1 at `/opt/ghidra` (project `re/ghidra`, program `dmcr.exe`; `tools/decomp.sh out.txt <addr|start-end>...`).

## What reverse engineering found (see re/*.txt)

- Multiplayer menu "connection type" (`FUN_00475710`) sets `DAT_006ddc74`: 0 IPX, 1 TCP/IP,
  2 TCP/IP with typed address (all three DirectPlay, broken under Wine), **3 = GSC native UDP
  with LAN broadcast discovery**, 4 = native UDP with typed IP (stored as 3 + `DAT_006ddc70=1`).
  When type >= 3, `DAT_006ddcb4 = 1` and DirectPlay is never created.
- Native UDP transport lives at 0x590000-0x592400 (`re/udp_module.txt`):
  - `FUN_00591ae0`: WSAStartup(2.2), then `FUN_00591bd0`: `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)`,
    `ioctlsocket(FIONBIO,1)`, `getsockopt(SOL_SOCKET, SO_MAX_MSG_SIZE)`, `bind(INADDR_ANY:34000)`.
  - `FUN_00591b30`: `gethostname` + `gethostbyname(own name)`: the list of the machine's own IPv4
    addresses; sent inside join packets and compared with the sender address (`ip`, port 34000)
    to mark peers "direct". **Our virtual IP must be what gethostbyname(own host) returns.**
  - Broadcast: `FUN_00590780` sends to `htonl(...)`/port 34000 with SO_BROADCAST (`setsockopt`)
    for discovery; peers are stored by IPv4 + port (table at this+0x2f8 / 0xcd/0xd1).
  - Receive: `FUN_005913a0`: `ioctlsocket(FIONREAD)`, then `recvfrom(sock, buf, 0x2000, 0, &from, &len)`.
    Polling only, no select() in this module.
  - Packet header 12 bytes: magic `0x47534370` ("pCSG" LE), u16 type, u32 seq (0 = unreliable),
    u16 player id. Reliable packets wait for ack with a 22.4 s timeout.
- A second UDP socket (`FUN_00590140`, used by `FUN_00408eb0`) sends 8-byte pings to a named
  host (gethostbyname/inet_addr). Probably lobby ping; leave it on the real network.
- dmcr.exe imports WS2_32 by ordinal: 1 accept, 2 bind, 3 closesocket, 4 connect,
  6 getsockname, 7 getsockopt, 8 htonl, 9 htons, 10 ioctlsocket, 11 inet_addr, 12 inet_ntoa,
  13 listen, 15 ntohs, 16 recv, 17 recvfrom, 18 select, 19 send, 20 sendto, 21 setsockopt,
  22 shutdown, 23 socket, 52 gethostbyname, 57 gethostname, 111 WSAGetLastError,
  115 WSAStartup, 116 WSACleanup, 151 __WSAFDIsSet (+ 109).

## Architecture

```
dmcr.exe --(IAT: ws2_32 ordinals)--> btwsteam hooks (inside ichat.dll proxy)
   UDP socket bound to :34000  ==>  virtual LAN 10.77.0.0/24
       sendto 10.77.0.N  -> ISteamNetworkingMessages to the lobby member with slot N
       sendto broadcast  -> every lobby member
       recvfrom          <- queue filled from Steam, from = 10.77.0.<sender slot>:<sender port>
   every other socket    ->  real ws2_32 (untouched)
Steam: steam_api.dll (flat API, loaded dynamically), friends-only lobby, invites, relay.
```

### Injection: `ichat.dll` proxy
- The game ships `bin/ichat.dll` (12 exports, C++ mangled names, imports KERNEL32, ADVAPI32,
  dmcr.exe, WSOCK32). dmcr.exe imports it by name. Installer renames the original to
  `ichat_orig.dll`; our `ichat.dll` forwards all 12 exports to it (.def forwarders) and on
  DLL_PROCESS_ATTACH patches dmcr.exe's IAT entries for WS2_32 (by ordinal) to our hooks.
  No Wine DLL override needed.

### Virtual addressing
- Slot = index of the member in the lobby, assigned by the host and published in lobby data
  (`slot_<steamid64>` = N, N in 1..254). Virtual IP = 10.77.0.N. Own slot known after joining.
- `gethostname` -> "btw-steam"; `gethostbyname("btw-steam")` (and of whatever gethostname
  returned) -> a hostent with the single address 10.77.0.<own slot>.
- `getsockname` on a hooked socket -> 10.77.0.<own slot>:<bound port>.
- Which sockets are hooked: UDP sockets that `bind()` to port 34000. Everything else is real.

### Wire format on Steam (channel 0, unreliable + NoNagle: the game has its own reliability)
```
u8  version = 1
u8  kind    : 0 = unicast, 1 = broadcast
u16 src_port (network order, the hooked socket's bound port)
u16 dst_port
payload (the game's UDP datagram, <= 0x2000 bytes)
```
Control channel 1, reliable: hello / keep-alive (1 s) / goodbye; peer silent 20 s is dropped.

### Steam session
- On start: `SteamAPI_InitFlat`; app id from `steam_appid.txt` (4850 when launched by Steam,
  fallback 480 Spacewar). Manual dispatch on a worker thread; received messages go into a
  locked queue that `recvfrom`/`ioctlsocket(FIONREAD)` read.
- Command line `+connect_lobby <id>` or env/arg `-steamjoin <id>` -> JoinLobby. Otherwise
  CreateLobby(friends-only, 7 max) at startup, SetRichPresence connect, so friends use
  "Join game" / "Invite to game" from the Steam friends list. Callback 333 / 337 -> leave and join.
- Only lobby members' messages are accepted (check membership on SessionRequest 1251).
- Log to `bin/btwsteam.log`.

### Testing on one PC
- Mock `steam_api.dll` with the same flat exports, localhost UDP between instances, fake SteamID
  from env `BTWSTEAM_MOCK_ID`, deterministic lobby id. Two isolated copies of `bin/` in
  `~/btw-test/a` and `~/btw-test/b` running under Wine/Proton.

## Layout
```
src/hook/       ichat.dll proxy: IAT hooks, virtual LAN, Steam session
src/mock/       mock steam_api.dll (tests only)
third_party/    steam_api.dll (Steamworks.NET Standalone 2025.164.1, Windows-x86)
tools/          decomp.sh, build.sh, install.sh, test launchers
re/             reverse-engineering outputs and Ghidra scripts
```
