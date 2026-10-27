# dmcr.exe networking: verification of DESIGN.md assumptions (2026-10-27)

Target: `bin/dmcr.exe` v1.35 (SHA-256 3923b0ff...), Ghidra project `re/ghidra`.
Evidence in `re/`: `ws_imports.txt` (every WS2_32 import, IAT slot, call sites; `scripts/WsXrefs.java`),
`ws_owners.txt` (string fingerprints per socket function; `scripts/FuncStrings.java`), `flow1.txt`,
`flow2.txt` (host/join/menu/lobby), `clan.txt`, `ping_408eb0.txt`, `scripts/DecompLong.java`.

UDP transport object: global at **0x005ba6e8** (ctor call 0x4044b5). Fields: `+0xbf` (DAT_005ba7a7)
is-host, `+0xc3` own player id, `+0x56` max players, `+0x68` game param, `+0xa7` accepting joins.

**Headline: DESIGN.md's "LAN broadcast" premise is wrong.** The native UDP transport has no
discovery, no broadcast, no SO_BROADCAST. It unicasts to a host IP string (`DAT_006ddd90`) that
normally comes from an internet lobby (GameSpy: dead; GSC-GAME.NET: disabled by a data file).
The transport works fine over a virtual LAN; the lobby step must be replaced, best inside our
`ichat.dll`.

## 1. WS2_32 imports

IAT `.rdata` 0x59e2b0-0x59e31c; callers use `call [IAT]` or `jmp [IAT]` thunks (0x5711b0-0x571210,
0x59a964-0x59a9a0, 0x551f40), so patching the IAT slot covers all. No GetProcAddress for winsock.

| Ord | Name | IAT | Native UDP (0x590400-0x5922f0) | ICMP ping (0x590140-0x5903f0) |
|---|---|---|---|---|
| 2 | bind | 0x59e2ec | 0x591c8b | – |
| 3 | closesocket | 0x59e2d4 | 0x591ca7 | 0x5901e1 |
| 7 | getsockopt | 0x59e2b4 | 0x591c3c (SO_MAX_MSG_SIZE, result unused) | – |
| 9 | htons | 0x59e2d0 | all `htons(34000)` | – |
| 10 | ioctlsocket | 0x59e2e4 | 0x591c07 FIONBIO, 0x5913c2 FIONREAD | 0x59019f, 0x590377 |
| 11 | inet_addr | 0x59e2cc | 0x590495 "0.0.0.0", **0x5906bd inet_addr(DAT_006ddd90) = host** | 0x59023b |
| 12 | inet_ntoa | 0x59e300 | 0x59120d (GameSpy room info only) | – |
| 17 | recvfrom | 0x59e30c | 0x5913fc | 0x5903b1 |
| 18 | select | 0x59e2dc | none | none |
| 20 | sendto | 0x59e310 | 0x5920ab, 0x591fb5 (retransmit) | 0x5902c9 |
| 21 | setsockopt | 0x59e318 | **none** | none |
| 23 | socket | 0x59e2c8 | 0x591bdc socket(2,2,17) | 0x590174 **socket(2,3,1) raw ICMP** |
| 52 | gethostbyname | 0x59e2b8 | 0x591b4e (own hostname) | 0x590256 "peerchat.gamespy.com" |
| 57 | gethostname | 0x59e2f8 | 0x591b3d (only caller) | – |
| 115/116 | WSAStartup/Cleanup | 0x59e2fc/0x59e2e0 | 0x591af3 / 0x591bba | 0x59015a / 0x5901ef |

Other users of sockets: GameSpy gstats (0x54e1a0-0x550300), httpc clan check (0x55d080-0x55f8ff),
GameSpy Peer SDK: peerchat, server browser, qr, pinger (0x57fe30-0x58ffxx), GameSpy Presence
(0x592400-0x59a9xx). Ordinal 109 = WSASetBlockingHook (server browser). DirectPlay goes via COM only.
csemu.dll (Intel laptop TDK) and ichat.dll (WSOCK32) have their own IATs.

## 2. Native UDP transport

- FUN_00591bd0: socket(UDP) -> ioctlsocket(FIONBIO) -> getsockopt(SO_MAX_MSG_SIZE) -> bind(INADDR_ANY:34000).
  Bind failure -> "SOCKERROR".
- No broadcast. FUN_00590780 is the client *leave* (type 10 to host `this+0x2c`). 255.255.255.255
  appears only if inet_addr fails (empty/invalid DAT_006ddd90 -> INADDR_NONE).
- Receive polled (FUN_004062a0 -> FUN_005913a0): FIONREAD, if 0 return; recvfrom(buf 0x2000).
  One datagram per call; caller loops.
- Host = `this+0x2c = inet_addr(DAT_006ddd90)`, port 34000 hard-coded. Other peers: address/port
  observed by recvfrom, stored in records (0x4f bytes from this+0xcd: +0xcd addr, +0xd1 port);
  host redistributes in type 9 peer lists; clients then send to each other directly (full mesh,
  FUN_00590c60).
- Port always 34000; no ip:port parsing.
- Header 12 bytes: +0 magic 0x47534370, +4 u16 type, +6 u32 seq (0 = unreliable), +10 u16 sender id.
  Live datagrams far below 8 KB. SYNC save transfer (FUN_0040a2e0/0040a730, 16438 bytes) is dead code.
- Reliability (FUN_00591fd0): resend every 2800 ms (FUN_00591e60), dropped after 9 tries (~25 s).
  0x5780 (22.4 s) only applies to wait=1, never used. Join waits up to 30.8 s for type 7/8.

## 3. Own-IP list (this+0x30, count this+0x50)

FUN_00591b30: gethostname(this+0xc, 0xff) + gethostbyname -> copies all h_addr_list entries, no
bound (only 8 fit). this+0xc is the 32-byte nick field: hostname >= 32 chars overflows.
Used only in: (1) join request (version 3, nick[32] +1, CCUID[22] +0x21, u16 count +0x38, IPs +0x3a);
host sets a "direct" flag (+0xd9) that nothing reads; (2) host's own record addr (cosmetic).
No subnet check, no IP-based host election. Identity = host-assigned id + **CCUID** (22 bytes,
`"%-8.8s-%8.8X-%4.4X"`, persisted in `HKCU\Software\GSC Game World\CCUID`, FUN_00590f80/00590eb0).
Shared CCUID between two instances -> joiner treated as existing record (maybe id 1 = host).

## 4. Flow

- Multi Player menu FUN_00483fb0 -> Deathmatch FUN_00472be0 / Historical FUN_00474090 ->
  connection menu FUN_00475710 (sets DAT_006ddc74; type 4 stored as 3 + DAT_006ddc70=1).
- Types 0-2: DirectPlay. Type 3 (DAT_006ddc70=0): GameSpy login FUN_00552620 + peerchat lobby
  FUN_00478a30(1). **Type 4**: FUN_00552620 -> FUN_00561d80 runs `Internet/cash/login.cmd`, which
  loads `LogDisabled.cml` ("Playing via GSC-GAME.NET is not supported in this version!", Exit =
  #CANCEL) -> returns false. If true, FUN_00478a30 just calls
  **`IChat.dll!Process_GSC_ChatWindow(bool, RoomInfo* = 0x6d88e8)`** (IAT 0x59e060, site 0x478a6b).
- Lobby result: 1 -> create, 2 -> join, 3 -> create variant (needs GameSpy room).
- After lobby (0x472c94): DAT_006ddcb4=1, CloseMPL, FUN_004068b0 -> FUN_00591ae0 (socket, bind,
  gethostbyname) **only now**.
- Create: FUN_00407220(RoomInfo+0x00 room name, RoomInfo+0x80 nick, 0, 1, u16 RoomInfo+0x124 max
  players) -> FUN_00590520 (host id 1, accepting, room name <= 28 chars) -> room UI FUN_0047d450(1,0).
- Join: RoomInfo+0xc0 (host IP string) -> DAT_006ddd90; FUN_00406d10(..., RoomInfo+0x80 nick, 1) ->
  FUN_00590610: type 6 reliable to host, wait 30.8 s for 7/8 -> FUN_0047d450(0,0) (or FUN_00482160
  for historical battle).
- Start: packet 0x037242f3 in FUN_00408a00; FUN_004070d0 sets +0xa7=2 (later joins refused, code 6);
  then StartGSCGame (IChat, 0x473ea9), StartIGame.
- Disconnect: transport none; room (FUN_00404ff0, "GNIP" pings): host drops peer silent > 20 s;
  client with host silent > 20 s shows NOSRCONN. Host close sends nothing. Client leave sends type 10.

Packet types (switch in FUN_005913a0): 1 ack; 3 game data (u16 len + data); 6 join request (refusal
codes: 1 not host, 2 full, 4 version != 3, 6 not accepting); 7 join accepted (own id, room name[32],
u32 game param); 8 join refused; 9 full peer table (FUN_005922f0); 10 peer leaves; 11 leave (never
sent); 13 reset (no sender); 14 nick change -> 16; 15 user data -> 17.

## 5. Reachability / `-adhoc`

`-adhoc` is only a csbtw.exe switch (Wlanapi ad-hoc Wi-Fi, SSID gsc_cossacks_btw, then runs dmcr.exe).
Connection menu buttons (labels are sprites in encrypted ALL.GSC; mapping inferred, confirm by screenshot):

| y | frames | DAT_006ddc74 | path | likely label |
|---|---|---|---|---|
| 0x106 | 0-2 | 0 | DirectPlay IPX | IPX |
| 0x120 | 3-5 | 1 | DirectPlay TCP/IP LAN | TCP/IP |
| 0x13a | 0xc-0xe | 2 | DirectPlay TCP/IP + address box | TCP/IP address |
| 0x154 | 0xf-0x11 | 4 -> 3 | GSC-GAME.NET login, then IChat (disabled) | GSC-GAME.NET |
| 0x16e | 0x14-0x16 | 3 | GameSpy login and lobby (dead) | Internet |

Bottom: 0xf9 create (x 0x5d), 0xfa join (x 0xee), 0xf6 back (x 0x17f).
Unlock: patch **0x55264e** `E8 2D F7 00 00` -> `B0 01 90 90 90` (FUN_00561d80 returns true), or
replace login.cmd/.cml.

IChat imports on the typed path: ChatProcess 0x59e050 (every room frame, 0x47f01d), ReportAliveState
0x59e044 (0x471884), SendVictoryState 0x59e048, StartGSCGame 0x59e058 (0x473ea9), LeaveGSCRoom
0x59e054, ReportGSCGame 0x59e064 (0x471ac2), GetGameID 0x59e068 (0x4c1382), SendPrivateMessage
0x59e05c (0x47876a), GoHomeAnyway 0x59e04c (each Multi Player menu open, 0x483fd2).
Process_GSC_ChatWindow(false, ...) is called from the room's extra button id 0x63 (0x481967).
Clan HTTP check (FUN_0055d660) only when nick starts with `[`.

## 6. ICMP ping socket

Raw ICMP echo to peerchat.gamespy.com every 1 s from 15 s into a UDP game; reply count only decides
drop attribution for stats (0x409807). Harmless without replies. Do not virtualize.

## Impact on the hook

1. No broadcast discovery; matchmaking comes from the Steam lobby. Map 255.255.255.255 to "drop"
   (or host slot) as a fallback only.
2. ichat.dll must implement IChat itself: `Process_GSC_ChatWindow(true, RoomInfo*)` waits for the
   Steam slot (pumping window messages), fills +0x00 room name (<= 27 chars), +0x80 nick (<= 31,
   not starting with `[`), +0x124 u16 max players, joiners +0xc0 "10.77.0.<host slot>"; returns 1
   owner / 2 member. `false` -> 0. Joiner must wait for a `host_ready` lobby flag set by the host
   after its call returned 1. ChatProcess, ReportAliveState, SendVictoryState, StartGSCGame,
   ReportGSCGame, LeaveGSCRoom, SendPrivateMessage, GetGameID (empty string) -> no-ops;
   GoHomeAnyway no-op or forward.
3. Unlock type 4: verify and patch 0x55264e, or replace login.cmd/.cml.
4. Socket created/bound only after the lobby call; socket/FIONBIO/getsockopt happen before bind:
   switch to virtual at bind(34000). Do not really bind 34000 (two local instances collide).
5. recvfrom source = stable 10.77.0.<slot>:34000; full-mesh unicast between any slots.
   gethostbyname fake: <= 8 addresses, hostname < 32 chars.
6. FIONREAD non-zero while queued; one datagram per recvfrom; WSAEWOULDBLOCK when empty;
   > 0x2000 -> copy 0x2000 + WSAEMSGSIZE.
7. No select/__WSAFDIsSet/getsockname/getpeername on the transport socket.
8. Game timeouts: 20 s room, ~25 s reliable. Steam silent-peer drop should be > 20 s (30 s) or not cut.
9. CCUID: separate per instance (separate Wine prefixes, or virtualize the registry value per SteamID).
10. Leave the ICMP socket alone. 11. GameSpy/httpc/IChat stay on the real network.
