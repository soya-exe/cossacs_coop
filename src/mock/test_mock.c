/*
 * test_mock.exe: two-process test of steam_api.dll (the mock) through the flat API only,
 * loaded with LoadLibrary like the hook does.
 *
 *   test_mock.exe host <lobbyfile>    creates a lobby and writes its id to <lobbyfile>
 *   test_mock.exe join <lobbyfile>    joins the id read from <lobbyfile>
 *
 * Env: BTWSTEAM_MOCK_ID     own mock id (also read by the DLL; default 1 for host, 2 for join)
 *      TEST_PEER_ID         the other instance's mock id (default 2 for host, 1 for join)
 *      TEST_JOIN_VIA_INVITE=1  join the lobby from callback 333 instead of the file
 *      TEST_EXPECT_LOSS=1      require that some unreliable messages were dropped
 *      TEST_SENDER_OFFSET=12   where to read the sender identity in SteamNetworkingMessage_t
 *      TEST_TIMEOUT_S=60
 *      TEST_MIN_JOIN_MS=0      require JoinLobby -> LobbyEnter to take at least this long (lag check)
 * Prints "RESULT: PASS" or "RESULT: FAIL ..." and exits 0 / 1.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "steam_flat.h"

#define ID_BASE    76561190000000000ULL
#define LOBBY_BASE 109775240000000000ULL
#define NMSG       100
#define BIG_SIZE   (300 * 1024)
#define MSG_MAGIC  0x47534D54u   /* "TMSG" */

#define X(r, n, p) static PFN_##n p_##n;
STEAM_FLAT_FUNCS(X)
#undef X

static ISteamUser *s_user; static ISteamMatchmaking *s_mm; static ISteamNetworkingMessages *s_nm;
static ISteamNetworkingUtils *s_nu; static ISteamFriends *s_fr;
static int is_host; static unsigned my_id, peer_id; static uint64_t my_sid, peer_sid;
static int sender_ofs = 12;
static char fails[4096]; static int nfail;

/* event state */
static uint64_t lobby; static SteamAPICall_t create_call, join_call;
static int got_created_result, got_created_bcast, got_enter_cb, got_enter_result, enter_response;
static int got_invite; static uint64_t invite_lobby, invite_friend;
static int peer_entered, peer_left, data_updates, session_requests, session_req_ok;
static int recv_count[NMSG], recv_rel, recv_unrel, last_rel_seq = -1, rel_order_bad, bad_sender, bad_payload;
static int got_big, big_ok, got_done;

static void fail(const char *fmt, ...)
{
    va_list ap; int n = (int)strlen(fails);
    if (n < (int)sizeof fails - 200) {
        va_start(ap, fmt);
        n += snprintf(fails + n, sizeof fails - n, " [");
        n += vsnprintf(fails + n, sizeof fails - n, fmt, ap);
        snprintf(fails + n, sizeof fails - n, "]");
        va_end(ap);
    }
    nfail++;
    printf("[%s] FAIL: ", is_host ? "host" : "join");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}
#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)
static void info(const char *fmt, ...)
{
    va_list ap;
    printf("[%s] ", is_host ? "host" : "join");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static uint64_t ident_sid(const void *p) { return p_SteamAPI_SteamNetworkingIdentity_GetSteamID64((SteamNetworkingIdentity *)p); }

static void handle_cb(int id, const void *p, int size)
{
    switch (id) {
    case k_iLobbyCreated: {
        const LobbyCreated_t *c = p;
        CHECK(size == sizeof *c, "513 size %d", size);
        got_created_bcast++;
        CHECK(c->m_eResult == 1 && c->m_ulSteamIDLobby == LOBBY_BASE + my_id, "513 broadcast result %d lobby %llu",
              c->m_eResult, (unsigned long long)c->m_ulSteamIDLobby);
        break; }
    case k_iLobbyEnter: {
        const LobbyEnter_t *e = p;
        CHECK(size == sizeof *e, "504 size %d", size);
        if (e->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess) got_enter_cb++;
        break; }
    case k_iLobbyChatUpdate: {
        const LobbyChatUpdate_t *u = p;
        CHECK(size == sizeof *u, "506 size %d", size);
        info("506 user=%llu flags=0x%x", (unsigned long long)u->m_ulSteamIDUserChanged, u->m_rgfChatMemberStateChange);
        if (u->m_ulSteamIDUserChanged == peer_sid && (u->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered)) peer_entered++;
        if (u->m_ulSteamIDUserChanged == peer_sid && (u->m_rgfChatMemberStateChange & k_EChatMemberStateChangeLeft)) peer_left++;
        break; }
    case k_iLobbyDataUpdate: {
        const LobbyDataUpdate_t *u = p;
        CHECK(size == sizeof *u, "505 size %d", size);
        CHECK(u->m_ulSteamIDLobby == lobby || !lobby, "505 for wrong lobby");
        data_updates++;
        break; }
    case k_iGameLobbyJoinRequested: {
        const GameLobbyJoinRequested_t *j = p;
        CHECK(size == sizeof *j, "333 size %d", size);
        got_invite++; invite_lobby = j->m_steamIDLobby; invite_friend = j->m_steamIDFriend;
        info("333 invite lobby=%llu friend=%llu", (unsigned long long)invite_lobby, (unsigned long long)invite_friend);
        break; }
    case k_iSteamNetworkingMessagesSessionRequest: {
        uint64_t who = ident_sid(p);    /* identity at offset 0 */
        CHECK(size == 136, "1251 size %d", size);
        session_requests++;
        info("1251 session request from %llu", (unsigned long long)who);
        if (who == peer_sid) {
            session_req_ok++;
            CHECK(p_SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(s_nm, (const SteamNetworkingIdentity *)p),
                  "AcceptSessionWithUser returned false");
        } else fail("1251 from unexpected %llu", (unsigned long long)who);
        break; }
    default: break;
    }
}

static void pump(void)
{
    HSteamPipe pipe = p_SteamAPI_GetHSteamPipe();
    CallbackMsg_t m;
    p_SteamAPI_ManualDispatch_RunFrame(pipe);
    while (p_SteamAPI_ManualDispatch_GetNextCallback(pipe, &m)) {
        if (m.m_iCallback == k_iSteamAPICallCompleted) {
            const SteamAPICallCompleted_t *d = (const void *)m.m_pubParam;
            bool failed = true;
            if (d->m_iCallback == k_iLobbyCreated && d->m_hAsyncCall == create_call) {
                LobbyCreated_t c;
                if (p_SteamAPI_ManualDispatch_GetAPICallResult(pipe, d->m_hAsyncCall, &c, sizeof c, k_iLobbyCreated, &failed) && !failed) {
                    got_created_result++;
                    lobby = c.m_ulSteamIDLobby;
                    CHECK(c.m_eResult == 1, "LobbyCreated result %d", c.m_eResult);
                    CHECK(lobby == LOBBY_BASE + my_id, "lobby id %llu", (unsigned long long)lobby);
                } else fail("GetAPICallResult(LobbyCreated) failed");
            } else if (d->m_iCallback == k_iLobbyEnter && d->m_hAsyncCall == join_call) {
                LobbyEnter_t e;
                if (p_SteamAPI_ManualDispatch_GetAPICallResult(pipe, d->m_hAsyncCall, &e, sizeof e, k_iLobbyEnter, &failed) && !failed) {
                    got_enter_result++;
                    enter_response = (int)e.m_EChatRoomEnterResponse;
                    CHECK(e.m_ulSteamIDLobby == lobby, "LobbyEnter lobby %llu", (unsigned long long)e.m_ulSteamIDLobby);
                } else fail("GetAPICallResult(LobbyEnter) failed");
            } else fail("unexpected call result callback %d", d->m_iCallback);
        } else handle_cb(m.m_iCallback, m.m_pubParam, m.m_cubParam);
        p_SteamAPI_ManualDispatch_FreeLastCallback(pipe);
    }
}

static uint64_t msg_sender(SteamNetworkingMessage_t *m) { return ident_sid((char *)m + sender_ofs); }

static void drain(void)
{
    SteamNetworkingMessage_t *msgs[32];
    int ch, n, i;
    for (ch = 0; ch < 3; ch++) {
        while ((n = p_SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(s_nm, ch, msgs, 32)) > 0) {
            for (i = 0; i < n; i++) {
                SteamNetworkingMessage_t *m = msgs[i];
                const uint8_t *data = *(void **)((char *)m + 0);
                int size = *(int32_t *)((char *)m + 4);
                uint64_t from = msg_sender(m);
                if (from != peer_sid) {
                    if (!bad_sender) info("message sender at offset %d decodes to %llu, expected %llu",
                                          sender_ofs, (unsigned long long)from, (unsigned long long)peer_sid);
                    bad_sender++;
                } else if (ch == 0) {
                    uint32_t hdr[3]; int seq, k, ok = size >= 12;
                    if (ok) memcpy(hdr, data, 12);
                    ok = ok && hdr[0] == MSG_MAGIC && hdr[1] < NMSG && hdr[2] == peer_id && size == 64 + (int)hdr[1] * 7;
                    seq = ok ? (int)hdr[1] : -1;
                    for (k = 12; ok && k < size; k++) ok = data[k] == (uint8_t)(seq * 31 + k);
                    if (!ok) { bad_payload++; }
                    else {
                        recv_count[seq]++;
                        if (seq % 2 == 0) {
                            recv_rel++;
                            if (seq <= last_rel_seq) rel_order_bad++;
                            last_rel_seq = seq;
                        } else recv_unrel++;
                    }
                } else if (ch == 2) {
                    int k, ok = size == BIG_SIZE;
                    for (k = 0; ok && k < size; k++) ok = data[k] == (uint8_t)(k * 7 + (k >> 9) + peer_id);
                    got_big++; big_ok += ok;
                } else if (ch == 1) {
                    if (size == 4 && !memcmp(data, "DONE", 4)) got_done++;
                }
                p_SteamAPI_SteamNetworkingMessage_t_Release(m);
            }
        }
    }
}

static void send_all(void)
{
    SteamNetworkingIdentity id; static uint8_t buf[64 + NMSG * 7]; uint8_t *big; int i, k, r;
    p_SteamAPI_SteamNetworkingIdentity_SetSteamID64(&id, peer_sid);
    CHECK(p_SteamAPI_SteamNetworkingIdentity_GetSteamID64(&id) == peer_sid, "identity round trip");
    for (i = 0; i < NMSG; i++) {
        int size = 64 + i * 7;
        uint32_t hdr[3] = { MSG_MAGIC, (uint32_t)i, my_id };
        memcpy(buf, hdr, 12);
        for (k = 12; k < size; k++) buf[k] = (uint8_t)(i * 31 + k);
        r = p_SteamAPI_ISteamNetworkingMessages_SendMessageToUser(s_nm, &id, buf, (uint32_t)size,
            (i % 2 == 0 ? k_nSteamNetworkingSend_Reliable : 0) | k_nSteamNetworkingSend_NoNagle, 0);
        CHECK(r == 1, "send %d returned %d", i, r);
        if (i % 10 == 9) { pump(); drain(); Sleep(1); }
    }
    big = malloc(600 * 1024);
    for (k = 0; k < BIG_SIZE; k++) big[k] = (uint8_t)(k * 7 + (k >> 9) + my_id);
    r = p_SteamAPI_ISteamNetworkingMessages_SendMessageToUser(s_nm, &id, big, BIG_SIZE, k_nSteamNetworkingSend_Reliable, 2);
    CHECK(r == 1, "300 KB send returned %d", r);
    r = p_SteamAPI_ISteamNetworkingMessages_SendMessageToUser(s_nm, &id, big, 512 * 1024 + 1, k_nSteamNetworkingSend_Reliable, 2);
    CHECK(r != 1, "512 KB + 1 send did not fail (%d)", r);
    free(big);
    r = p_SteamAPI_ISteamNetworkingMessages_SendMessageToUser(s_nm, &id, "DONE", 4, k_nSteamNetworkingSend_Reliable, 1);
    CHECK(r == 1, "DONE send returned %d", r);
    info("sent %d messages + 300 KB + DONE", NMSG);
}

static int load(void)
{
    HMODULE h = LoadLibraryA("steam_api.dll");
    if (!h) { printf("cannot load steam_api.dll (%lu)\n", GetLastError()); return 0; }
#define X(r, n, p) if (!(p_##n = (PFN_##n)(void (*)(void))GetProcAddress(h, #n))) { printf("missing export %s\n", #n); return 0; }
    STEAM_FLAT_FUNCS(X)
#undef X
    return 1;
}

static uint64_t read_lobby_file(const char *path)
{
    FILE *f = fopen(path, "r"); unsigned long long v = 0;
    if (!f) return 0;
    if (fscanf(f, "%llu", &v) != 1) v = 0;
    fclose(f);
    return v;
}

static void check_members(void)
{
    int n = p_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(s_mm, lobby), i, saw_me = 0, saw_peer = 0;
    for (i = 0; i < n; i++) {
        uint64_t m = p_SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(s_mm, lobby, i);
        saw_me += m == my_sid; saw_peer += m == peer_sid;
    }
    info("lobby %llu: %d members, owner %llu", (unsigned long long)lobby, n,
         (unsigned long long)p_SteamAPI_ISteamMatchmaking_GetLobbyOwner(s_mm, lobby));
    CHECK(n == 2 && saw_me == 1 && saw_peer == 1, "member list: n=%d me=%d peer=%d", n, saw_me, saw_peer);
    CHECK(p_SteamAPI_ISteamMatchmaking_GetLobbyOwner(s_mm, lobby) == (is_host ? my_sid : peer_sid), "owner is not the host");
}

#define WAIT_UNTIL(cond, what) do { \
    while (!(cond)) { \
        if (GetTickCount() - t0 > timeout_ms) { fail("timeout waiting for %s", what); goto done; } \
        if (bad_sender) { fail("messages with an undecodable sender"); goto done; } \
        pump(); drain(); Sleep(2); \
    } } while (0)

int main(int argc, char **argv)
{
    char v[64]; char err[1024]; DWORD t0; DWORD timeout_ms = 60000; int expect_loss = 0, via_invite = 0, i, missing_rel = 0, min_join_ms = 0;
    DWORD join_t = 0;
    const char *lobbyfile;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3 || (strcmp(argv[1], "host") && strcmp(argv[1], "join"))) {
        printf("usage: test_mock host|join <lobbyfile>\n"); return 2;
    }
    is_host = !strcmp(argv[1], "host"); lobbyfile = argv[2];
    if (GetEnvironmentVariableA("TEST_SENDER_OFFSET", v, sizeof v)) sender_ofs = atoi(v);
    if (GetEnvironmentVariableA("TEST_TIMEOUT_S", v, sizeof v)) timeout_ms = (DWORD)atoi(v) * 1000;
    if (GetEnvironmentVariableA("TEST_EXPECT_LOSS", v, sizeof v)) expect_loss = atoi(v);
    if (GetEnvironmentVariableA("TEST_JOIN_VIA_INVITE", v, sizeof v)) via_invite = atoi(v);
    if (GetEnvironmentVariableA("TEST_MIN_JOIN_MS", v, sizeof v)) min_join_ms = atoi(v);
    my_id = is_host ? 1 : 2; peer_id = is_host ? 2 : 1;
    if (GetEnvironmentVariableA("BTWSTEAM_MOCK_ID", v, sizeof v)) my_id = (unsigned)atoi(v);
    if (GetEnvironmentVariableA("TEST_PEER_ID", v, sizeof v)) peer_id = (unsigned)atoi(v);
    my_sid = ID_BASE + my_id; peer_sid = ID_BASE + peer_id;
    t0 = GetTickCount();

    if (!load()) { printf("RESULT: FAIL [load]\n"); return 1; }
    CHECK(p_SteamAPI_IsSteamRunning(), "IsSteamRunning");
    if (p_SteamAPI_InitFlat(err) != 0) { printf("InitFlat failed: %s\nRESULT: FAIL [init]\n", err); return 1; }
    p_SteamAPI_ManualDispatch_Init();
    s_user = p_SteamAPI_SteamUser_v023(); s_mm = p_SteamAPI_SteamMatchmaking_v009();
    s_fr = p_SteamAPI_SteamFriends_v018(); s_nm = p_SteamAPI_SteamNetworkingMessages_SteamAPI_v002();
    s_nu = p_SteamAPI_SteamNetworkingUtils_SteamAPI_v004();
    CHECK(s_user && s_mm && s_fr && s_nm && s_nu, "null interface");
    CHECK(p_SteamAPI_ISteamUser_GetSteamID(s_user) == my_sid, "GetSteamID %llu, want %llu",
          (unsigned long long)p_SteamAPI_ISteamUser_GetSteamID(s_user), (unsigned long long)my_sid);
    CHECK(p_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(s_nu, k_ESteamNetworkingConfig_RecvMaxMessageSize, 32 << 20),
          "SetGlobalConfigValueInt32");
    {
        /* GetPersonaName: "MockPlayer<id>" unless BTWSTEAM_MOCK_PERSONA is set */
        char want[64]; const char *got = p_SteamAPI_ISteamFriends_GetPersonaName(s_fr);
        snprintf(want, sizeof want, "MockPlayer%u", my_id);
        if (!GetEnvironmentVariableA("BTWSTEAM_MOCK_PERSONA", NULL, 0))
            CHECK(got && !strcmp(got, want), "GetPersonaName \"%s\", want \"%s\"", got ? got : "(null)", want);
        else
            CHECK(got && got[0], "GetPersonaName empty with BTWSTEAM_MOCK_PERSONA set");
    }

    if (is_host) {
        DeleteFileA(lobbyfile);
        create_call = p_SteamAPI_ISteamMatchmaking_CreateLobby(s_mm, k_ELobbyTypeFriendsOnly, 4);
        CHECK(create_call != 0, "CreateLobby returned 0");
        WAIT_UNTIL(got_created_result && got_created_bcast && got_enter_cb, "LobbyCreated x2 + LobbyEnter");
        info("lobby %llu created (703:%d 513:%d 504:%d)", (unsigned long long)lobby, got_created_result, got_created_bcast, got_enter_cb);
        CHECK(p_SteamAPI_ISteamMatchmaking_SetLobbyData(s_mm, lobby, "name", "btwtest"), "SetLobbyData name");
        CHECK(!strcmp(p_SteamAPI_ISteamMatchmaking_GetLobbyData(s_mm, lobby, "name"), "btwtest"), "GetLobbyData on owner");
        p_SteamAPI_ISteamFriends_SetRichPresence(s_fr, "connect", "+connect_lobby 1");
        {
            char tmp[MAX_PATH]; FILE *f;
            snprintf(tmp, sizeof tmp, "%s.tmp", lobbyfile);
            f = fopen(tmp, "w"); fprintf(f, "%llu\n", (unsigned long long)lobby); fclose(f);
            MoveFileExA(tmp, lobbyfile, MOVEFILE_REPLACE_EXISTING);
        }
        WAIT_UNTIL(peer_entered, "peer to enter the lobby (506)");
        check_members();
        CHECK(p_SteamAPI_ISteamMatchmaking_SetLobbyData(s_mm, lobby, "phase", "go"), "SetLobbyData phase");
        /* the peer talks first: its first message must raise 1251 here and be held until accepted */
        WAIT_UNTIL(session_req_ok, "1251 session request from the peer");
        send_all();
        WAIT_UNTIL(got_done && got_big, "peer's DONE and big message");
        drain();
        WAIT_UNTIL(peer_left, "peer to leave (506 flag 0x2)");
        CHECK(p_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(s_mm, lobby) == 1, "member count after leave");
        CHECK(session_requests == 1, "host saw %d session requests, want 1", session_requests);
    } else {
        if (via_invite) {
            WAIT_UNTIL(got_invite, "333 GameLobbyJoinRequested");
            lobby = invite_lobby;
            CHECK(invite_friend == peer_sid, "333 friend %llu", (unsigned long long)invite_friend);
        } else {
            WAIT_UNTIL((lobby = read_lobby_file(lobbyfile)) != 0, "lobby id file");
        }
        info("joining %llu", (unsigned long long)lobby);
        /* the host's lobby may not exist yet when the invite fires: retry on DoesntExist */
        for (;;) {
            got_enter_result = 0; enter_response = 0;
            join_t = GetTickCount();
            join_call = p_SteamAPI_ISteamMatchmaking_JoinLobby(s_mm, lobby);
            WAIT_UNTIL(got_enter_result, "LobbyEnter call result");
            join_t = GetTickCount() - join_t;
            if (enter_response == k_EChatRoomEnterResponseSuccess) break;
            info("join response %d, retrying", enter_response);
            Sleep(500);
        }
        WAIT_UNTIL(got_enter_cb, "504 LobbyEnter callback");
        info("join took %lu ms", (unsigned long)join_t);
        CHECK((int)join_t >= min_join_ms, "join took %lu ms, lag should make it >= %d", (unsigned long)join_t, min_join_ms);
        check_members();
        CHECK(!strcmp(p_SteamAPI_ISteamMatchmaking_GetLobbyData(s_mm, lobby, "name"), "btwtest"), "GetLobbyData name on joiner");
        CHECK(!p_SteamAPI_ISteamMatchmaking_SetLobbyData(s_mm, lobby, "x", "y"), "non-owner SetLobbyData succeeded");
        send_all();
        WAIT_UNTIL(!strcmp(p_SteamAPI_ISteamMatchmaking_GetLobbyData(s_mm, lobby, "phase"), "go"), "lobby data phase=go");
        CHECK(data_updates >= 2, "joiner saw %d 505 LobbyDataUpdate, want >= 2", data_updates);
        WAIT_UNTIL(got_done && got_big, "peer's DONE and big message");
        drain();
        CHECK(session_requests == 0, "joiner saw %d session requests, want 0 (it sent first)", session_requests);
        p_SteamAPI_ISteamMatchmaking_LeaveLobby(s_mm, lobby);
        CHECK(p_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(s_mm, lobby) == 0, "member count after own leave");
    }

    for (i = 0; i < NMSG; i += 2) if (recv_count[i] != 1) missing_rel++;
    for (i = 0; i < NMSG; i++) CHECK(recv_count[i] <= 1, "message %d received %d times", i, recv_count[i]);
    info("received reliable %d/%d, unreliable %d/%d, big %d ok %d, bad payload %d, bad sender %d",
         recv_rel, NMSG / 2, recv_unrel, NMSG / 2, got_big, big_ok, bad_payload, bad_sender);
    CHECK(missing_rel == 0, "%d reliable messages missing", missing_rel);
    CHECK(rel_order_bad == 0, "reliable messages out of order");
    CHECK(bad_payload == 0 && bad_sender == 0, "bad payload %d / bad sender %d", bad_payload, bad_sender);
    CHECK(got_big == 1 && big_ok == 1, "300 KB message: got %d ok %d", got_big, big_ok);
    if (expect_loss) CHECK(recv_unrel > 0 && recv_unrel < NMSG / 2, "loss not visible: %d/%d unreliable arrived", recv_unrel, NMSG / 2);
    else CHECK(recv_unrel == NMSG / 2, "unreliable %d/%d without loss", recv_unrel, NMSG / 2);

done:
    p_SteamAPI_Shutdown();
    if (nfail) { printf("RESULT: FAIL%s\n", fails); return 1; }
    printf("RESULT: PASS (%.1f s)\n", (GetTickCount() - t0) / 1000.0);
    return 0;
}
