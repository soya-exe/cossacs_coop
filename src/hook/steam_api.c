/*
 * Dynamic loading of steam_api.dll (flat C API, __cdecl, interface pointer first).
 * No SDK headers, no import library: GetProcAddress for each export below. A
 * CSteamID argument is a plain uint64 in the flat API (8 bytes on the stack).
 *
 * This list is the contract with the mock steam_api.dll used for tests: do not use
 * anything that is not here.
 */
#include "btwsteam.h"
#include <stdio.h>
#include <string.h>

btw_sapi_t S;

typedef struct {
    const char *name;
    size_t      off;
    int         required;
} sym_t;

#define R(field, name) { name, offsetof(btw_sapi_t, field), 1 }
#define O(field, name) { name, offsetof(btw_sapi_t, field), 0 }
static const sym_t k_syms[] = {
    R(InitFlat,                    "SteamAPI_InitFlat"),
    R(Shutdown,                    "SteamAPI_Shutdown"),
    O(IsSteamRunning,              "SteamAPI_IsSteamRunning"),
    R(GetHSteamPipe,               "SteamAPI_GetHSteamPipe"),
    R(MD_Init,                     "SteamAPI_ManualDispatch_Init"),
    R(MD_RunFrame,                 "SteamAPI_ManualDispatch_RunFrame"),
    R(MD_GetNextCallback,          "SteamAPI_ManualDispatch_GetNextCallback"),
    R(MD_FreeLastCallback,         "SteamAPI_ManualDispatch_FreeLastCallback"),
    R(MD_GetAPICallResult,         "SteamAPI_ManualDispatch_GetAPICallResult"),
    R(SteamUser,                   "SteamAPI_SteamUser_v023"),
    R(User_GetSteamID,             "SteamAPI_ISteamUser_GetSteamID"),
    R(SteamMatchmaking,            "SteamAPI_SteamMatchmaking_v009"),
    R(MM_CreateLobby,              "SteamAPI_ISteamMatchmaking_CreateLobby"),
    R(MM_JoinLobby,                "SteamAPI_ISteamMatchmaking_JoinLobby"),
    R(MM_LeaveLobby,               "SteamAPI_ISteamMatchmaking_LeaveLobby"),
    R(MM_SetLobbyData,             "SteamAPI_ISteamMatchmaking_SetLobbyData"),
    R(MM_GetLobbyData,             "SteamAPI_ISteamMatchmaking_GetLobbyData"),
    R(MM_SetLobbyJoinable,         "SteamAPI_ISteamMatchmaking_SetLobbyJoinable"),
    R(MM_GetLobbyOwner,            "SteamAPI_ISteamMatchmaking_GetLobbyOwner"),
    R(MM_GetNumLobbyMembers,       "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers"),
    R(MM_GetLobbyMemberByIndex,    "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex"),
    O(MM_InviteUserToLobby,        "SteamAPI_ISteamMatchmaking_InviteUserToLobby"),
    R(SteamFriends,                "SteamAPI_SteamFriends_v018"),
    R(FR_SetRichPresence,          "SteamAPI_ISteamFriends_SetRichPresence"),
    O(FR_ClearRichPresence,        "SteamAPI_ISteamFriends_ClearRichPresence"),
    O(FR_ActivateGameOverlayInviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog"),
    R(SteamNetworkingMessages,     "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"),
    R(NM_SendMessageToUser,        "SteamAPI_ISteamNetworkingMessages_SendMessageToUser"),
    R(NM_ReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"),
    R(NM_AcceptSessionWithUser,    "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"),
    R(NM_CloseSessionWithUser,     "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser"),
    R(Msg_Release,                 "SteamAPI_SteamNetworkingMessage_t_Release"),
    R(Ident_SetSteamID64,          "SteamAPI_SteamNetworkingIdentity_SetSteamID64"),
    R(Ident_GetSteamID64,          "SteamAPI_SteamNetworkingIdentity_GetSteamID64"),
    O(SteamNetworkingUtils,        "SteamAPI_SteamNetworkingUtils_SteamAPI_v004"),
    O(NU_SetGlobalConfigValueInt32, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32"),
};
#undef R
#undef O

int btw_sapi_load(const char *dir)
{
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%ssteam_api.dll", dir);
    memset(&S, 0, sizeof S);
    S.dll = LoadLibraryA(path);
    if (!S.dll) {
        btw_log("steam: cannot load %s (error %lu)", path, (unsigned long)GetLastError());
        return -1;
    }
    int missing = 0;
    for (size_t i = 0; i < sizeof k_syms / sizeof k_syms[0]; i++) {
        FARPROC p = GetProcAddress(S.dll, k_syms[i].name);
        if (!p) {
            btw_log("steam: %s export %s missing", k_syms[i].required ? "required" : "optional",
                    k_syms[i].name);
            missing += k_syms[i].required;
        }
        /* function pointers are stored by offset; memcpy avoids cast warnings */
        memcpy((char *)&S + k_syms[i].off, &p, sizeof p);
    }
    if (missing) {
        btw_log("steam: %s lacks %d required export(s); too old? (need the flat API)", path, missing);
        FreeLibrary(S.dll);
        memset(&S, 0, sizeof S);
        return -1;
    }
    btw_log("steam: loaded %s", path);
    return 0;
}
