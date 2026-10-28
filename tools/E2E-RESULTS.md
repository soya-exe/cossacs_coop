# End-to-end test: two game copies over the hook and the mock Steam

Date: 2026-10-27, 21:03-21:17 local time. Copy a is the host (mock id 1, nick HostA). Copy b
joins (mock id 2, nick JoinB, `BTWSTEAM_JOIN=109775240000000001`). Both run headless in
gamescope with system wine 11.20 and their own prefixes. The CCUIDs differ:
a `SOYA0000-01709F32-43B4`, b `SOYA0000-0177D168-4396`.
Build: `tools/build.sh` and `tools/build_mock.sh`. Install: `tools/test_install.sh a b --hook --mock`.
Screenshots and saved logs are in `~/btw-test/e2e/`.

## Summary

| Step | Result |
|---|---|
| 1 Build + install | PASS |
| 2 b joins a's room | PASS: both rooms list HostA and JoinB, ping about 30 ms, lost 0 % |
| 3 Ready, start, play at least 2 min | PASS: same map, game time in step (0:02:51 on both), commands and chat work both ways, no pause or desync, 0 drops |
| 4 Same with lag 80 ms + loss 5 % on both | PASS: room shows ping 192, lost 10 %; the game starts and runs 3+ min (0:03:14 / 0:03:13), no pause |
| 5 Kill b mid-game | PASS: a shows the game's own handling (PAUSE "JoinB: Not responding", then "Player JoinB has left the game"), and a goes on alone |
| Mutation: recvfrom source port 34001 | DETECTED: the join fails. Restored, rebuilt, and the join works again |

No bugs were found and no source files changed. `verify_install.sh`: install unchanged. All
instances are stopped.

## What the UI needs (found in this run)

- Joiner: Multiplayer (290,262) -> Deathmatch (290,288) -> GSC-Game.Net Internet Game
  (300,352) -> **Create** (157,489), the same button as the host. The hook decides from lobby
  membership: `Process_GSC_ChatWindow` returns 2 for a member. The Join button (0xfa) was not needed.
- Ready: the joiner clicks **Start** (912,605). Its status bar says "Click here when you are ready
  to play the network game". Clicking the joiner's ✗ in the Ready column does nothing. The host's
  Ready cell shows a percentage for a few seconds after the room opens (probably random-map
  preparation), then ✓. Once both show ✓, the host clicks Start (912,605).
- In game: box-select with left drag, move order with right click (XTEST button 3); chat with Return, text, Return.

## Step 2: join (run 1, no lag)

b's btwsteam.log:
```
21:04:25.139 LOBBY JOINED 109775240000000001, owner 76561190000000001, 2 member(s), btwsteam=1 ver=1
21:04:25.139 peers: own slot 0 -> 2 (virtual IP 10.77.0.2)
21:04:25.139 steam: lobby 109775240000000001: host_ready=1 (the host's game room exists)
21:04:46.569 ichat: member of lobby 109775240000000001: JOIN host 10.77.0.1:34000 as "JoinB" (we are 10.77.0.2, returns 2)
21:04:46.569 vlan: first sendto on hooked socket 352: 10.77.0.1:34000, 74 bytes      <- join request (type 6)
21:04:46.579 vlan: first recvfrom on hooked socket 352: 16 bytes from 10.77.0.1
21:04:46.587 ichat: ChatProcess called (no-op; logged once)                          <- b is in the room UI
```
Screenshots: `s2-room-a.png` and `s2-room-b.png` (HostA red, JoinB blue, "max ping: 34 ... lost:0%").

## Step 3: game (run 1, no lag)

- Ready: `s3-ready-a.png`, `s3-ready-b.png`. The host clicked Start at 21:05:38. Both logs show
  `ichat: StartGSCGame` (a 21:05:38.961, b 21:05:38.585).
- About 6 s later both are on the same random map, and both lists show "HostA (7.20) / JoinB (7.20)"
  with game time 0:00:06: `s3-ingame-a-6s.png`, `s3-ingame-b-6s.png`.
- Commands: each side box-selected its 18 peasants and gave a move order. Both groups moved
  (`s3-moved-a.png`, `s3-moved-b.png`, game time 0:00:24 on both).
- Chat both ways (it goes through game data): "JoinB: hello from B" shows on a
  (`s3-chat-from-b-on-a.png`), and "HostA: hello from A" shows on b (`s3-chat-from-a-on-b.png`).
- After 2:49 of wall time both show game time **0:02:51** with no pause or desync dialog and
  nearly equal food (458 / 456): `s3-run-a-2m.png`, `s3-run-b-2m.png`.
- Counters at that point (every drop counter 0, no broadcasts):
```
a 21:08:27 stats: tx uni 10061 bcast-dropped 0 bytes 241979 | tx drop nonvlan 0 noslot 0 dead 0 queue 0 steamerr 0 | rx 10060 bytes 241300 reads 10060 | rx drop unknown 0 bad 0 nosock 0 qfull 0 noslot 0 | ctrl tx 243 rx 243
b 21:08:25 stats: tx uni 9931 bcast-dropped 0 bytes 238148 | tx drop ... all 0 | rx 9931 bytes 238803 reads 9931 | rx drop ... all 0 | ctrl tx 241 rx 240
```
Logs: `run1-a-btwsteam.log`, `run1-b-btwsteam.log`.

## Step 4: lag 80 ms + loss 5 % (run 2)

`BTWSTEAM_MOCK_LAG_MS=80 BTWSTEAM_MOCK_LOSS=0.05` on both. The mock logs confirm
`lag=80ms loss=0.050` on id 1 and id 2.

- The join works. The game's reliable join exchange needed about one retransmit
  (first reply 21:10:26.611, room UI 21:10:29.425, about 2.8 s, the game's resend interval).
- Room: "max ping: 192 ... lost:10%" (`s4-ready-a.png`, `s4-ready-b.png`). This matches
  2 x 80 ms plus about 30 ms base, and 1 - 0.95² ≈ 10 % round-trip loss.
- Start at 21:11:04 works on both (`s4-ingame-a.png`, `s4-ingame-b.png`, game time 0:00:08 on both).
- Moves on both sides and chat both ways work (`s4-cmd-a.png`: "JoinB: lag hello from B" on a;
  `s4-chat-from-a-on-b.png`: "HostA: lag check two" on b). The first chat from a had already
  faded when b's screenshot was taken, so it was resent and caught within 1.5 s.
- After 3:16 of wall time: game time **0:03:14 / 0:03:13**, no pause, food 337 / 336
  (`s4-run-a-3m.png`, `s4-run-b-3m.png`). Game time keeps up with wall time, so there were no
  noticeable stalls.
- Counters (drops 0; mock loss happens below the hook, so it does not show here):
```
a 21:14:20 stats: tx uni 1710 bytes 44508 | tx drop all 0 | rx 1644 bytes 44575 reads 1644 | rx drop all 0 | ctrl tx 249 rx 250
b 21:14:18 stats: tx uni 1696 bytes 46175 | tx drop all 0 | rx 1601 bytes 41803 reads 1601 | rx drop all 0 | ctrl tx 248 rx 247
```
  With 192 ms ping the game sends far fewer datagrams than without lag (about 7/s against about
  45/s in run 1, averaged from join to the last stats line), so its lockstep batches more per packet. Logs: `run2-lag-a-btwsteam.log`, `run2-lag-b-btwsteam.log`.

## Step 5: leave (run 1, at game time about 3:10)

`tools/stop_instance.sh b` at 21:08:44.6.
- 21:08:49.555, a's log: the mock lobby drops b after 5 s of keep-alive silence (506 flag 0x4).
  The hook clears `slot_76561190000000002`, and `peers: member ... (slot 2) left`. After that,
  game sends to 10.77.0.2 are dropped as `noslot` (130 by 21:09:27; this is expected).
- +5 s and +13 s: a shows the game's **PAUSE** box "HostA: Ready / JoinB: Not responding.", and
  the game is frozen at tick [209] (`s5-a-5s.png`, `s5-a-13s.png`).
- About +35 s: "Player JoinB has left the game." Game time runs again (0:03:49) and a plays on
  alone (`s5-a-35s.png`). a's log: `SendVictoryState called` at 21:09:26.
  Log: `run1-a-btwsteam-after-leave.log`.

## Mutation check

`src/hook/vlan.c` recvfrom was changed to report source port `BTW_GAME_PORT + 1` (34001).
The DLL was rebuilt and reinstalled, and the pair was run with no lag.
- The host accepts the join request and lists JoinB with "JoinB : 0, lost:???" (`mut-a.png`).
  Its replies go to 34001, and b drops them all: `rx 71 ... reads 0 | rx drop ... nosock 71`
  (`mut-b-btwsteam.log`).
- b's join fails, and b is back at "Choose a Connection" (`mut-b.png`). The test therefore
  depends on the stable `:34000` source port.
- Restored from a copy and rebuilt (`grep MUTATION` finds 0 lines). After reinstalling, a re-run
  joins normally ("max ping: 32, lost:0%", `restored-a.png`).

## Attempt log

1. Run 1 (no lag): joined, readied, started, ran 3 min, then the b-kill leave test. Everything passed on the first try.
   The only exploration was the ready control: the Ready-column click did nothing, and Start worked.
2. Run 2 (lag 80 + loss 0.05): passed on the first try.
3. Mutation run: failed as expected. The restore run passed.

## Remaining problems / notes

- None blocking. Notes:
  - The leave depends on the game's own timeouts: PAUSE "Not responding" after about 5 s, the
    player dropped after about 30 s. The hook adds nothing, as designed.
  - In-game sync was judged from equal game time and tick counters, the absence of the game's
    pause and desync dialogs, near-equal resources, and chat delivery. No unit of one player was
    seen on the other screen, because the starts are far apart and the map is under fog.
  - The joiner must already be in the lobby (`BTWSTEAM_JOIN`, an invite, or Join game) before it
    clicks GSC-Game.Net. Otherwise it owns its own lobby and creates a room (as documented).
  - The test copies a and b keep the hook and mock installed (`tools/test_install.sh a b --restore` reverts them).

## Real Steam, two accounts (2026-10-28)

Host: the owner, Arch Linux + Proton, btwsteam 0.1.0 installed with installer/install.sh, real
`steam_api.dll` (Steamworks.NET 2025.164.1). Joiner: a friend's Steam account on another PC.
Result reported by the owner: both players entered the room and played. Host log
(`bin/btwsteam.log`) shows:

- `LOBBY CREATED` (friends-only, 7), host slot 1 = 10.77.0.1; the friend entered the lobby
  (callback 506 0x1), got slot 2 = 10.77.0.2.
- `message sender identity found at offset 12`: the 32-bit SteamNetworkingMessage_t layout
  confirmed on real Steam.
- Game 1: StartGSCGame 02:01:42, game traffic until ~02:01:58, LeaveGSCRoom 02:02:03.
- Game 2: StartGSCGame 02:03:47, ran until the friend left the lobby at 02:05:23:
  ~1150 datagrams each way, all drop counters 0 (no `steamerr`, `qfull`, `unknown`).
- After the friend left, the host's sends to 10.77.0.2 are counted as `noslot` (expected) and the
  game closed its socket at 02:05:39.
- Friend left and re-entered the lobby twice between the games (02:02:13 left, 02:02:41
  entered, 02:02:45 left, 02:03:14 entered); cause unknown without the friend's log.
- Noise: `call result ... (callback 1296 / 2101) failed` lines are Steam's own API calls that the
  hook's dispatcher tries to fetch; harmless. Since 0.1.1 the hook fetches only its own
  CreateLobby / JoinLobby results and counts the rest (`foreign call results` in `stats:`).
- The joiner's `hello ... (they say slot 0, table says 2)`: the hello was queued before the joiner
  had its slot (only logged, nothing reads it). Since 0.1.1 the hello waits for the own slot.

## Nick from the Steam name, mock (2026-10-28, btwsteam 0.1.1)

`tools/test_install.sh a b --hook --mock`; a: `BTWSTEAM_MOCK_PERSONA="Иван Петрович"`, b:
`BTWSTEAM_MOCK_PERSONA="[TAG] Ölaf"` and `BTWSTEAM_JOIN=109775240000000001`; `BTWSTEAM_NICK` unset.
Screenshots and logs: `~/btw-test/e2e-nick/`.

- Logs: a `steam: persona name "Иван Петрович" (in game: "Ivan Petrovich")`, `CREATE room
  "Ivan Petrovich's game", nick "Ivan Petrovich"`; b `persona name "[TAG] Ölaf" (in game: "TAG
  Olaf")`, `JOIN host 10.77.0.1:34000 as "TAG Olaf"`.
- Room (`room-a-host.png`, `room-a-with-b.png`, `room-b-joiner.png`): both list
  **Ivan_Petrovich** and **TAG_Olaf**. The game draws the space in a nick as `_`.
- Ready + Start (`ready-a.png`): both started (StartGSCGame on both), and both in-game lists show
  `Ivan_Petrovich (7.20)` / `TAG_Olaf (7.20)` at game time 0:00:13 (`ingame-a.png`, `ingame-b.png`).
  ~2150 datagrams each way, all drop counters 0.
- Hellos: `hello from ...02 (they say slot 2, table says 2)` and `... slot 1, table says 1`.
