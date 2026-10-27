# Mock steam_api.dll (tests only)

A 32-bit stand-in for `steam_api.dll` so that two or more game processes on one PC (Wine,
Proton or Windows) can test btwsteam without two Steam accounts. It exports exactly the 36
flat functions in `steam_flat.h` with the real signatures (cdecl). Every process is one fake
Steam user and runs a small "Steam backend" on UDP `127.0.0.1:(47000 + id)`. There is no
shared memory, so separate Wine prefixes work too.

Build: `tools/build_mock.sh` -> `build/mock/steam_api.dll` and `build/mock/test_mock.exe`.
Test: `tools/test_mock.sh` runs two instances of `test_mock.exe` under wine and prints PASS/FAIL.
Scenarios: join by id; lag 100 ms + loss 0.3 with the join coming from the 333 invite; a
mutation that reads the sender at offset 16 and must fail. The test uses mock ids 901/902
by default so it can't collide with game instances on 1/2; set `TEST_HOST_ID` /
`TEST_JOIN_ID` to change them, and `WINEPREFIX` / `TEST_DIR` for where it runs.
Test-exe knobs: `TEST_SENDER_OFFSET`, `TEST_EXPECT_LOSS`, `TEST_JOIN_VIA_INVITE`,
`TEST_MIN_JOIN_MS`, `TEST_TIMEOUT_S`, `TEST_PEER_ID` (see the top of `test_mock.c`).
To use it in a test install, copy `build/mock/steam_api.dll` over the real one in that copy's `bin/`.

## Environment variables

| Variable | Meaning |
|---|---|
| `BTWSTEAM_MOCK_ID` | Player number 1..1000 (default 1). SteamID = 76561190000000000 + id; UDP port 47000 + id. Two processes with the same id: the second one's `SteamAPI_InitFlat` returns 1. |
| `BTWSTEAM_MOCK_LAG_MS` | Delay, in ms, before this process handles any incoming packet (lobby traffic and messages). It is one-way: set it on both processes to get an RTT of 2 x lag. |
| `BTWSTEAM_MOCK_LOSS` | 0..1: the chance that an **unreliable** send is silently dropped (the send still returns OK). Sends with the Reliable flag (8) are never dropped. Applied by the sender. |
| `BTWSTEAM_MOCK_INVITE` | A lobby id. Callback 333 `GameLobbyJoinRequested_t {lobby, friend = lobby's owner}` fires 3 s after `InitFlat`, as if the user accepted a Steam invite. |
| `BTWSTEAM_MOCK_TIMEOUT_MS` | Lobby keep-alive timeout (default 5000). See "Lobbies". |

The log goes to `steam_mock_<id>.log` next to the exe, appending, with timestamps.

## Behaviour

**Init:** `SteamAPI_InitFlat` returns 0 (it returns 1 only if the UDP port can't be bound).
`IsSteamRunning` returns true. The interface getters return distinct non-null dummy pointers
after `InitFlat` and NULL before it (as real Steam does). Each method logs a WARNING if its
first argument is not the pointer from the matching getter. `GetHSteamPipe` is 1.

**Callbacks** go through manual dispatch only. Call results arrive as 703 and are fetched with
`GetAPICallResult`, which needs the exact callback id and struct size. The mock rejects
mismatches (real Steam probably does too, but that is unverified). `RunFrame` is a no-op
because the backend runs on its own thread, so callbacks queue up whether or not you pump.

**Lobbies** (lobby id = 109775240000000000 + creator's id; the lobby lives in the owner's process):
- `CreateLobby` queues, in order: 703 (call result `LobbyCreated_t`, result 1), 513 `LobbyCreated_t`
  again (real Steam sends it twice), then 504 `LobbyEnter_t`. Calling it while in a lobby
  leaves that lobby first.
- `JoinLobby` sends a join request to port 47000 + (lobby - base). On the joiner, the owner's
  reply queues 505 (lobby data), 703 (call result `LobbyEnter_t`) and then 504 `LobbyEnter_t`.
  On the owner and every other member it queues 506 `LobbyChatUpdate_t` (flag 0x1). If there
  is no reply within the timeout, the result is `LobbyEnter_t` with response 2 (DoesntExist).
  Full lobby -> 4; lobby not joinable -> 3.
- `SetLobbyData` works only for the owner. Non-members get false, as on real Steam. A value
  that changes queues 505 (`m_ulSteamIDMember == lobby`) on every member, owner included.
  Setting the same value again sends no callback. An empty value deletes the key.
  Limits: key <= 255, value <= 8192, and the whole lobby state <= ~60 KB.
- `LeaveLobby`: the others get 506 with flag 0x2. When the owner leaves, the first remaining
  member becomes owner, and later join requests to the old owner's port are redirected to the
  new one for as long as the old process is alive.
- Keep-alive: the owner and the members ping each other every second. A member silent for
  `BTWSTEAM_MOCK_TIMEOUT_MS` is removed with 506 flag 0x4 (disconnected). If the owner goes
  silent, the members drop it (0x4) and promote the first remaining member. This is how a
  killed game process shows up.
- `GetLobbyData` returns "" if the key is missing or you are not in that lobby. The returned
  pointer stays valid for the next 7 calls.
- `InviteUserToLobby`, `SetRichPresence`, `ClearRichPresence` and
  `ActivateGameOverlayInviteDialog` only log.
- One lobby per process.

**Networking messages:**
- `SendMessageToUser` sends straight to the target's port. Messages over 60000 bytes are split
  into fragments and reassembled. Sends over 512 KB return 8 (`k_EResultInvalidParam`). An
  unknown or non-mock identity also returns 8.
- The first message from a peer you have neither sent to nor accepted is held, and raises
  1251 `SteamNetworkingMessagesSessionRequest_t` (identity at offset 0) once.
  `AcceptSessionWithUser` releases the held messages in order. Sending to a peer accepts its
  session implicitly, as on real Steam. `AcceptSessionWithUser` returns false when nothing
  is pending. `CloseSessionWithUser` drops that peer's queued messages and resets the session.
- `ReceiveMessagesOnChannel` returns `SteamNetworkingMessage_t*` laid out like the real
  32-bit DLL: `m_pData` at 0, `m_cbSize` at 4, `m_identityPeer` at **12**, `m_nChannel` at
  184, `m_pfnRelease` at 180. `SteamAPI_SteamNetworkingMessage_t_Release` calls
  `m_pfnRelease`, as the real export does. A hook that reads the sender at 16 gets 0 here
  and on real Steam.
- `SteamNetworkingIdentity` uses the real layout (checked by disassembling the real DLL):
  type 16 at offset 0, size 8 at offset 4, SteamID64 at offset **8**. Use `SetSteamID64` /
  `GetSteamID64`.
- Order: one sender's messages arrive in send order (channels share one stream).
- Reliable delivery is not real: reliable messages are never dropped by `BTWSTEAM_MOCK_LOSS`,
  but there is no retransmission. Localhost UDP loses packets only if the receive buffer
  overflows, and the backend thread drains the socket all the time.
- `SetGlobalConfigValueInt32` logs and returns true.

## Not covered

- 1252 SessionFailed and 337 GameRichPresenceJoinRequested never fire.
- Real Steam's ordering of 504 against 513 on create, and of 505 against 504 on join, is not
  verified. The mock always uses the orders above, so make the hook's handlers idempotent and
  independent of order.
- `GetLobbyMemberData`, friend lists and the other exports aren't implemented. Add them here
  and to `STEAM_FLAT_FUNCS` in `steam_flat.h` if the hook starts using them.
