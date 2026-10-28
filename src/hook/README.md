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
     - **lobby owner**: fills +0x00 room name (`<nick>'s game`, at most 26 chars), +0x80 nick,
       and +0x124 max players = 7. It returns 1 (create room) and publishes lobby data `host_ready=1`.
     - **member**: waits for `host_ready=1`, then fills the nick, max players and +0xc0
       `10.77.0.<owner slot>`. It returns 2 (join that host on :34000).
     - Esc while waiting, or no Steam: returns 0.
   - The nick is `BTWSTEAM_NICK`, else the Steam persona name, else `Player<slot>`. Both names
     are UTF-8 and go through `btw_nick_sanitize` (`nick.c`), because the nick travels as
     `nick[32]` in the game's join request and is drawn with the game's bitmap fonts:
     Cyrillic (Russian, Ukrainian) is transliterated to Latin, Latin letters lose diacritics,
     other non-ASCII (emoji, CJK) is dropped, spaces collapse, at most 31 chars, and no leading
     `[` (clan HTTP check): `[TAG] Ölaf` -> `TAG Olaf`, `Иван Петрович` -> `Ivan Petrovich`.
     The game shows spaces in nicks as `_`. Unit test: `tools/test_nick.sh` (host gcc).
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
   10 ms, or at once when the game sends. Game data is also received on the game thread (see
   "Threads" and "Performance").
   - After init it reads the persona name (`ISteamFriends::GetPersonaName`, optional export)
     for the nick.
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
     control messages (hello / keep-alive every 1 s / goodbye). The hello goes to each member
     once our own slot is known, so it carries the real slot; nothing depends on it.
   - Call results (703) are fetched only for the CreateLobby / JoinLobby calls we issued.
     Steam's own (callbacks 1296, 2101, ...) are counted as `foreign call results` in `stats:`.
   - A member silent for 30 s gets nothing forwarded until it talks again. Nothing else
     happens: the game has its own 20 s timeout, and we never inject packets.
   - The message sender is read at offset 12 of `SteamNetworkingMessage_t` (real 32-bit DLL),
     with 16 as the fallback. The log says which one was found.

6. **Perf patches** (`perf.c`, both in DllMain, both in memory only; numbers in
   `../../tools/PERF-RESULTS.md`, "Implemented (0.1.3)"). Each one checks its bytes first and,
   on any mismatch, logs it and leaves the code alone.
   - **novsync** (Wine/Proton only, detected by `ntdll!wine_get_version`). csemu.dll's render
     thread presents with `Flip(NULL, DDFLIP_WAIT)`; under wined3d the vsync'd flip makes its
     next back-buffer `Lock` spin (3-93 % of a core). We check csemu.dll (file size 357056,
     PE timestamp 4d26ff86, image size 5c000, checksum 59355) and the 15 bytes at csemu+0x317f
     (`8B 08 8B 51 2C 83 C4 0C 6A 01 6A 00 50 FF D2`), then write `09` (`DDFLIP_WAIT |
     DDFLIP_NOVSYNC`) at csemu+0x3188. csemu.dll is a static import of dmcr.exe, so it is
     mapped before our DllMain; its render thread starts much later. Pacing stays with csemu's
     limiter on the game thread. On Windows nothing is changed (real fullscreen DirectDraw
     would tear).
   - **nospin**. The in-game frame function 0x4b1530 busy-waits in two loops that call
     `ProcessMessages` (0x4717a0) and the game clock (0x5250e0): the MP tick wait
     (0x4b158c-0x4b15e7, deadline in EDI, BL = "render extra frames") and the SP end-of-frame
     wait (0x4b1964-0x4b199b, `speed/2` ms after the last frame, 0x6dd7e4 / 0x8c0e54, or while
     0x8c0e14 is set). After checking dmcr.exe 1.35 (base 0x400000, timestamp 4d2c8aaf) and the
     bytes of both loops, `ProcessMessages` and the clock function, the two `call
     ProcessMessages` (0x4b15d7 `E8 C4 01 FC FF`, 0x4b1964 `E8 37 FE FB FF`) are pointed at
     wrappers. When the loop is only waiting (MP: BL=0), a wrapper waits about 1 ms in
     `MsgWaitForMultipleObjectsEx(QS_ALLINPUT, MWMO_INPUTAVAILABLE)` (it returns at once for
     input) and then calls the real `ProcessMessages`. The loop conditions are the game's own.
     A timed wait starts only if it ends before the game clock can reach the deadline. Under
     Wine `GetTickCount` is the monotonic clock as of the last wineserver request (it lags by
     0-16 ms) and `QueryPerformanceCounter` is the same clock read now, so the wrapper knows
     the time left exactly; it needs the longest recent wait + 0.36 ms. The worker checks the
     two clocks and the wait length once at start (Wine 11 returns at once from a 1 ms
     `MsgWait`, so a 2 ms timeout is used there; it waits ~1 ms). On Windows the timed waits
     stay off: `GetTickCount` steps by ~15.6 ms and a 1 ms wait may take 15.6 ms, so a wait
     could move a lockstep tick; only the paused wait (no deadline) sleeps there. The game
     clock's offset is read directly: calling 0x5250e0 would slew the MP clock offset.
   - `perf:` lines every 10 s while a game runs: game frames (0x8c0e5c), wrapper calls and
     waits, time waited, waits ended by input, `late` (the fresh clock passed the deadline
     during a wait; expected 0) and the current wait length.

Threads: callbacks (manual dispatch), lobby calls and sends happen on the worker thread. The
one exception is receiving game data: when the game polls its socket (`ioctlsocket(FIONREAD)`
or `recvfrom`) and our queue for it is empty, the game thread calls
`ISteamNetworkingMessages::ReceiveMessagesOnChannel(0)` itself. That call is thread-safe
(GameNetworkingSockets takes its global lock in every ISteamNetworkingMessages call); a
receive lock serialises it with the worker's receive so datagrams reach the game in the
order Steam returned them, and the game thread only `TryEnter`s it, so it never waits for
the worker. It runs at most once per 500 us and not after Steam shut down. Otherwise the
game thread only touches our queues and tables, under one critical section.

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
| `BTWSTEAM_NICK=<name>` | player name instead of the Steam name (UTF-8, sanitized like it; see 1.) |
| `BTWSTEAM_INVITE=1` | open the overlay invite dialog once the lobby exists |
| `BTWSTEAM_INVITE_IDS=<id>,<id>` | `InviteUserToLobby` these SteamIDs once the lobby exists |
| `BTWSTEAM_APPID=<n>` | set `SteamAppId`/`SteamGameId` before init (e.g. 480) |
| `BTWSTEAM_NOSTEAM=1` | no Steam; the lobby window returns 0 |
| `BTWSTEAM_NOPATCH=1` | do not patch the GSC-GAME.NET login |
| `BTWSTEAM_DISABLE=1` | no hooks, no patch, no Steam |
| `BTWSTEAM_DEBUG=1` | log every datagram |
| `BTWSTEAM_TIMING=1` | log latency percentiles every 10 s (`timing:` lines, see "Performance") |
| `BTWSTEAM_GAME_THREAD_RX=0` | receive only on the worker (every 10 ms), as before 0.1.2 |
| `BTWSTEAM_NOVSYNC=0` | keep csemu's vsync'd flip (Wine/Proton only; "How it works" 6) |
| `BTWSTEAM_NOSPIN=0` | keep the game's busy-wait loops ("How it works" 6) |

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
- a `stats:` line of counters every 10 s;
- the perf patches (`novsync:`, `nospin:`) and, while a game runs, a `perf:` line every 10 s.

## Performance (latency)

Where the time goes, from `BTWSTEAM_TIMING=1` (two copies on one PC over the mock Steam,
numbers in `tools/E2E-RESULTS.md`, "Latency"):

- **Send** (`tx`: game `sendto` -> `SendMessageToUser`): p50 30 us, p95 ~130 us. The game
  thread queues the datagram and wakes the worker with an event. Sending directly from the
  game thread would save these ~30 us but would put Steam's send (and its global lock) inside
  the game's frame, so sends stay on the worker. `NoNagle` is set on every data message, so
  `k_ESteamNetworkingConfig_NagleTime` (12) does not apply; the send rate configs
  (`SendRateMin` 10, `SendRateMax` 11) do not matter at ~2.5 KB/s. No config value is changed
  besides the 4 MB send/receive buffers (9, 47): the mock cannot measure Steam's own configs,
  and none of them is a latency knob for this traffic.
- **Receive, hook** (`rxpoll`: Steam got it -> we took it out of Steam; `rxhook`: the game's
  first poll after Steam got it -> the `recvfrom` that delivered it). Before 0.1.2 only the
  worker received, every 10 ms, so a datagram that came in just before the game's poll waited
  for the game's next frame. Since 0.1.2 the game thread fetches from Steam whenever it polls
  an empty queue: `rxhook` is 10 us at p50 and 20 us at p95; the room's ping on one PC went
  from 28-30 ms to 17-19 ms.
- **Receive, game** (`rxwait`: Steam got it -> the game's next poll): the game reads its
  socket once per frame (`gap` p95 ~18 ms at ~55 fps here), so a datagram waits 0-18 ms (p50
  ~8 ms) for the next frame. This is the game's own loop and the biggest remaining share;
  only a change in the game (polling the socket more often than once per frame) could cut it.
- **Cost:** the worker still wakes every 10 ms (about 0.7 % of one core, unchanged); the
  game-thread receives take ~2 ms per second in total (p50 10-20 us per call).

`timing:` line fields: `tx`, `txcall` (duration of `SendMessageToUser`), `rxpoll`, `rxgame`
(our queue -> the game's `recvfrom`), `rxtotal` (Steam -> `recvfrom`), `rxwait`, `rxhook`,
`gap` (between two FIONREAD polls of the game), `gtcall` (one game-thread receive), each as
`n p50 p95 max` in microseconds, then the game's poll rate, the game-thread receive count and
time, and the CPU of the worker thread and the process. `rxpoll` needs
`ISteamNetworkingUtils::GetLocalTimestamp` (optional export) to compare with
`SteamNetworkingMessage_t::m_usecTimeReceived` (offset 160 in the 32-bit struct).
`tools/net_latency.sh` runs a two-copy room + match and summarises these lines with
`tools/timing_summary.py`.
