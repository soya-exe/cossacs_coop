# src/hook: our `ichat.dll` (btwsteam)

Lets Cossacks: Back to War play its native UDP multiplayer over Steam. The Steam lobby takes
the place of the dead internet lobby. Lobby members get a virtual address `10.77.0.<slot>`,
and their datagrams travel over `ISteamNetworkingMessages` (Valve relay, no port forwarding).
The game's netcode does not change. Background: `../../re/FINDINGS.md`, `../../DESIGN.md`.

## How it works

1. **IChat replaced** (`ichat.c`, `ichat.def`). `dmcr.exe` imports `IChat.dll` by name. We
   export the same 12 MSVC-mangled names (ordinals 1..12) and implement them ourselves. The
   installer keeps the original as `ichat_orig.dll`, but we never load or call it.
   - `Process_GSC_ChatWindow(true, RoomInfo*)` is the "lobby". It waits, pumping window
     messages, until the Steam lobby is ready and our slot is known. Then:
     - **lobby owner**: fills +0x00 room name (`<nick>'s game`), +0x80 nick, and +0x124 max
       players = 7. It returns 1 (create room) and publishes lobby data `host_ready=1`.
     - **member**: waits for `host_ready=1`, then fills the nick, max players and +0xc0
       `10.77.0.<owner slot>`. It returns 2 (join that host on :34000).
     - Esc while waiting, or no Steam: returns 0.
   - `Process_GSC_ChatWindow(false, ...)` (the chat button in the room) returns 0.
     `GetGameID` writes "". Everything else is a no-op (logged once).
2. **Menu unlock** (`ichat.c`). The GSC-GAME.NET connection type (4th button) normally fails
   its login (`FUN_00561d80`, which loads `LogDisabled.cml`). In DllMain we check that VA
   0x55264e holds `E8 2D F7 00 00` and replace it with `B0 01 90 90 90` (`mov al,1`). If the
   bytes differ we log it and leave them alone.
3. **IAT hooks** (`iat.c`). In DllMain we patch `dmcr.exe`'s WS2_32 imports, which are by
   ordinal: 2 bind, 3 closesocket, 6 getsockname, 10 ioctlsocket, 17 recvfrom, 20 sendto,
   21 setsockopt, 23 socket, 52 gethostbyname, 57 gethostname. The worker re-checks them every
   second. Other modules and other sockets (GameSpy, ICMP ping) stay on the real network.
4. **Virtual LAN** (`vlan.c`). The game creates its UDP socket only after the lobby call:
   socket, FIONBIO and getsockopt come first, then `bind(INADDR_ANY:34000)`. That bind makes
   the socket virtual. The real socket is bound to `127.0.0.1:0`, never to 34000, so several
   copies fit on one PC. On a virtual socket:
   - `sendto(10.77.0.N)` sends to the member with slot N; `10.77.0.<own>` loops back.
     Anything else is dropped and logged. This includes 255.255.255.255, which only appears
     when the host IP string was bad.
   - `recvfrom` pops one queued datagram (queue of 1024; the oldest is dropped when full).
     The source is always `10.77.0.<sender slot>:34000`. An empty queue gives
     `WSAEWOULDBLOCK`. A datagram larger than the buffer is truncated with `WSAEMSGSIZE` and
     consumed. `MSG_PEEK` works.
   - `ioctlsocket(FIONREAD)` returns the size of the next datagram, or 0.
   - `getsockname` returns `10.77.0.<own>:34000`. `gethostname` returns `btw-steam`, which
     must stay under 32 chars because the game copies it into a 32-byte field.
     `gethostbyname` of that name, of NULL/"" or of the real host name returns exactly one
     address, `10.77.0.<own slot>`.
5. **Steam** (`steam_api.c`, `session.c`, `peers.c`). A worker thread loads `steam_api.dll`
   from the exe folder: flat API only, through `GetProcAddress`, and never from DllMain. It
   writes `steam_appid.txt` (4850) if the file is missing. Manual callback dispatch runs every
   10 ms, or at once when the game sends.
   - With no join target, it creates a friends-only lobby for 7 players. It sets lobby data
     `btwsteam=1`, `ver=1` and rich presence `connect=+connect_lobby <id>`.
   - The owner assigns slots in lobby data: `slot_<steamid64>` = N. The owner prefers 1,
     existing slots never move, and the keys of members who left are cleared. Everyone reads
     the table back on events 504, 505 and 506.
   - Invites (333) and "Join game" (337) make it leave the current lobby and join the new one.
     So a joiner must accept the invite before clicking GSC-GAME.NET; otherwise it is the
     owner of its own lobby and creates a room.
   - Session requests (1251) are accepted only from lobby members. A request from someone
     not yet listed is held for 15 s.
   - Channel 0 carries game datagrams: unreliable + NoNagle, with the 6-byte header
     `u8 ver=1, u8 kind, u16 src_port BE, u16 dst_port BE`. Channel 1 carries reliable
     control messages (hello / keep-alive every 1 s / goodbye).
   - A member silent for 30 s gets nothing forwarded until it talks again. Nothing else
     happens: the game has its own 20 s timeout, and we never inject packets.
   - The message sender is read at offset 12 of `SteamNetworkingMessage_t` (real 32-bit DLL),
     with 16 as the fallback. The log says which one was found.

Threads: every call into `steam_api.dll` happens on the worker thread. The game thread only
touches our queues and tables, under one critical section.

## Build

`tools/build.sh` (mingw-w64 `i686-w64-mingw32-gcc`, C11, `-Wall -Wextra`) writes
`build/ichat.dll`. It checks the 12 exports in ordinal order and that the only imports are
KERNEL32, USER32, ADVAPI32, WS2_32 and msvcrt. It links `msvcrt.dll` instead of the UCRT with
`-mcrtdll=msvcrt-os`.

## Use

Install with `tools/test_install.sh <copy> --hook --mock` for tests. By hand: rename `ichat.dll`
to `ichat_orig.dll`, then copy ours and `steam_api.dll` into `bin/`. In the game, go to
Multiplayer -> Deathmatch -> **GSC-Game.Net Internet Game** (4th button).

| Environment / argument | Effect |
|---|---|
| `+connect_lobby <id>`, `-steamjoin <id>`, `BTWSTEAM_JOIN=<id>` | join that lobby at start |
| `BTWSTEAM_NICK=<name>` | player name (ASCII; leading `[` removed; default `Player<slot>`) |
| `BTWSTEAM_INVITE=1` | open the overlay invite dialog once the lobby exists |
| `BTWSTEAM_INVITE_IDS=<id>,<id>` | `InviteUserToLobby` these SteamIDs once the lobby exists |
| `BTWSTEAM_APPID=<n>` | set `SteamAppId`/`SteamGameId` before init (e.g. 480) |
| `BTWSTEAM_NOSTEAM=1` | no Steam; the lobby window returns 0 |
| `BTWSTEAM_NOPATCH=1` | do not patch the GSC-GAME.NET login |
| `BTWSTEAM_DISABLE=1` | no hooks, no patch, no Steam |
| `BTWSTEAM_DEBUG=1` | log every datagram |

## Log

`btwsteam.log` sits next to `dmcr.exe`; the previous run is kept as `btwsteam.prev.log`. Each
line is `HH:MM:SS.mmm [thread id] message` and goes straight to disk. It records:

- the IAT and login patches and the game's CCUID;
- each IChat call;
- every tracked UDP socket and each bind decision;
- the first send and receive per socket and per peer;
- lobby events (`LOBBY CREATED <id>` / `LOBBY JOINED <id>`, `host_ready`) and slot table
  changes;
- Steam errors;
- a `stats:` line of counters every 10 s.
