/*
 * steam_flat.h: the subset of the Steamworks flat C API (steam_api_flat.h) used by
 * btwsteam, for the 32-bit Windows steam_api.dll (Steamworks.NET Standalone 2025.164.1,
 * SHA-256 fdafb7c65ec7d4182b42bf92cc8671c68e4dc547cc6e613899d2618a47eb65da).
 *
 * No SDK headers needed. Shared by the mock steam_api.dll (src/mock) and the hook (src/hook).
 *
 * Calling convention: every function is __cdecl. CSteamID / uint64 / SteamAPICall_t are
 * passed by value as 8 bytes on the stack and returned in EDX:EAX. "bool" is 1 byte
 * (returned in AL). Interface methods take the interface pointer (from the getter) first.
 * Parameter lists come from Steamworks.NET 2025.164.1 autogen/NativeMethods.cs (which
 * mirrors steam_api_flat.h); SteamAPI_InitFlat (not in NativeMethods.cs) is
 * "ESteamAPIInitResult SteamAPI_InitFlat(SteamErrMsg *pOutErrMsg)", SteamErrMsg = char[1024].
 *
 * Usage:
 *   - dynamic loading (hook): STEAM_FLAT_FUNCS(X) gives one X(ret, name, params) per export;
 *     e.g.  #define X(r, n, p) typedef r (__cdecl *PFN_##n) p;  STEAM_FLAT_FUNCS(X) #undef X
 *   - the mock defines STEAM_FLAT_EXPORT before including to get dllexport prototypes.
 */
#ifndef BTW_STEAM_FLAT_H
#define BTW_STEAM_FLAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t  HSteamPipe;
typedef int32_t  HSteamUser;
typedef uint64_t SteamAPICall_t;   /* 0 = k_uAPICallInvalid */
typedef uint64_t CSteamID_u64;     /* flat API "uint64_steamid" */
typedef int32_t  EResult;          /* 1 = k_EResultOK, 2 = Fail, 8 = InvalidParam */

/* opaque interface pointers */
typedef struct ISteamUser ISteamUser;
typedef struct ISteamMatchmaking ISteamMatchmaking;
typedef struct ISteamFriends ISteamFriends;
typedef struct ISteamNetworkingMessages ISteamNetworkingMessages;
typedef struct ISteamNetworkingUtils ISteamNetworkingUtils;

/* ---- constants ---------------------------------------------------------------------- */
enum {
    k_ESteamAPIInitResult_OK = 0, k_ESteamAPIInitResult_FailedGeneric = 1,
    k_ESteamAPIInitResult_NoSteamClient = 2, k_ESteamAPIInitResult_VersionMismatch = 3
};
enum { k_ELobbyTypePrivate = 0, k_ELobbyTypeFriendsOnly = 1, k_ELobbyTypePublic = 2,
       k_ELobbyTypeInvisible = 3 };
enum { k_EChatRoomEnterResponseSuccess = 1, k_EChatRoomEnterResponseDoesntExist = 2,
       k_EChatRoomEnterResponseNotAllowed = 3, k_EChatRoomEnterResponseFull = 4,
       k_EChatRoomEnterResponseError = 5 };
enum { k_EChatMemberStateChangeEntered = 0x1, k_EChatMemberStateChangeLeft = 0x2,
       k_EChatMemberStateChangeDisconnected = 0x4, k_EChatMemberStateChangeKicked = 0x8,
       k_EChatMemberStateChangeBanned = 0x10 };
enum { k_nSteamNetworkingSend_Unreliable = 0, k_nSteamNetworkingSend_NoNagle = 1,
       k_nSteamNetworkingSend_NoDelay = 4, k_nSteamNetworkingSend_Reliable = 8 };
enum { k_ESteamNetworkingIdentityType_Invalid = 0, k_ESteamNetworkingIdentityType_SteamID = 16 };
enum { k_cbMaxSteamNetworkingSocketsMessageSizeSend = 512 * 1024 };
/* ESteamNetworkingConfigValue (subset) */
enum { k_ESteamNetworkingConfig_SendBufferSize = 9, k_ESteamNetworkingConfig_RecvBufferSize = 47,
       k_ESteamNetworkingConfig_RecvBufferMessages = 48, k_ESteamNetworkingConfig_RecvMaxMessageSize = 49,
       k_ESteamNetworkingConfig_RecvMaxSegmentsPerPacket = 50 };

/* callback ids */
enum {
    k_iGameLobbyJoinRequested = 333,          /* GameLobbyJoinRequested_t */
    k_iGameRichPresenceJoinRequested = 337,   /* GameRichPresenceJoinRequested_t */
    k_iLobbyEnter = 504,                      /* LobbyEnter_t */
    k_iLobbyDataUpdate = 505,                 /* LobbyDataUpdate_t */
    k_iLobbyChatUpdate = 506,                 /* LobbyChatUpdate_t */
    k_iLobbyCreated = 513,                    /* LobbyCreated_t */
    k_iSteamAPICallCompleted = 703,           /* SteamAPICallCompleted_t */
    k_iSteamNetworkingMessagesSessionRequest = 1251,
    k_iSteamNetworkingMessagesSessionFailed = 1252
};

/* ---- structs (Windows: callback structs are #pragma pack(8), i.e. natural alignment) ---- */
typedef struct CallbackMsg_t {
    HSteamUser m_hSteamUser;
    int32_t    m_iCallback;
    uint8_t   *m_pubParam;
    int32_t    m_cubParam;
} CallbackMsg_t;                                                    /* 16 bytes */

typedef struct SteamAPICallCompleted_t {
    SteamAPICall_t m_hAsyncCall;
    int32_t        m_iCallback;
    uint32_t       m_cubParam;
} SteamAPICallCompleted_t;                                          /* 16 */

typedef struct LobbyCreated_t {
    EResult  m_eResult;                     /* 0 */
    uint64_t m_ulSteamIDLobby;              /* 8 */
} LobbyCreated_t;                                                   /* 16 */

typedef struct LobbyEnter_t {
    uint64_t m_ulSteamIDLobby;              /* 0 */
    uint32_t m_rgfChatPermissions;          /* 8 */
    bool     m_bLocked;                     /* 12 */
    uint32_t m_EChatRoomEnterResponse;      /* 16, 1 = success */
} LobbyEnter_t;                                                     /* 24 */

typedef struct LobbyDataUpdate_t {
    uint64_t m_ulSteamIDLobby;              /* 0 */
    uint64_t m_ulSteamIDMember;             /* 8: == lobby for lobby data, else member data */
    uint8_t  m_bSuccess;                    /* 16 */
} LobbyDataUpdate_t;                                                /* 24 */

typedef struct LobbyChatUpdate_t {
    uint64_t m_ulSteamIDLobby;              /* 0 */
    uint64_t m_ulSteamIDUserChanged;        /* 8 */
    uint64_t m_ulSteamIDMakingChange;       /* 16 */
    uint32_t m_rgfChatMemberStateChange;    /* 24: k_EChatMemberStateChange* */
} LobbyChatUpdate_t;                                                /* 32 */

typedef struct GameLobbyJoinRequested_t {
    uint64_t m_steamIDLobby;                /* 0 */
    uint64_t m_steamIDFriend;               /* 8 */
} GameLobbyJoinRequested_t;                                         /* 16 */

typedef struct GameRichPresenceJoinRequested_t {
    uint64_t m_steamIDFriend;
    char     m_rgchConnect[256];
} GameRichPresenceJoinRequested_t;                                  /* 264 */

/*
 * SteamNetworkingIdentity: 136 bytes, alignment 1 in the real headers (pack 1).
 * Verified by disassembling the real 32-bit DLL's SetSteamID64/GetSteamID64:
 *   offset 0 m_eType (16 = SteamID), offset 4 m_cbSize (8), offset 8 the SteamID64.
 * Prefer SetSteamID64/GetSteamID64 over touching the bytes.
 */
typedef struct SteamNetworkingIdentity {
    uint8_t m_bytes[136];
} SteamNetworkingIdentity;

typedef struct SteamNetworkingMessagesSessionRequest_t {
    SteamNetworkingIdentity m_identityRemote;   /* 0 */
} SteamNetworkingMessagesSessionRequest_t;                          /* 136 */

/*
 * SteamNetworkingMessage_t as the real 32-bit steam_api.dll lays it out. The identity is at
 * 12 (it has alignment 1), NOT 16. m_pfnRelease at 180 (0xB4) is confirmed by the real DLL's
 * SteamAPI_SteamNetworkingMessage_t_Release, which is "jmp [ecx+0xB4]".
 */
typedef struct SteamNetworkingMessage_t SteamNetworkingMessage_t;
struct SteamNetworkingMessage_t {
    void    *m_pData;                       /* 0 */
    int32_t  m_cbSize;                      /* 4 */
    uint32_t m_conn;                        /* 8 */
    SteamNetworkingIdentity m_identityPeer; /* 12 */
    /* 4 bytes padding */
    int64_t  m_nConnUserData;               /* 152 */
    int64_t  m_usecTimeReceived;            /* 160 */
    int64_t  m_nMessageNumber;              /* 168 */
    void   (*m_pfnFreeData)(SteamNetworkingMessage_t *);  /* 176 */
    void   (*m_pfnRelease)(SteamNetworkingMessage_t *);   /* 180 */
    int32_t  m_nChannel;                    /* 184 */
    int32_t  m_nFlags;                      /* 188 */
    int64_t  m_nUserData;                   /* 192 */
    uint16_t m_idxLane;                     /* 200 */
    uint16_t _pad1__;
};                                                                  /* 208 */

#define STEAMNETMSG_OFS_DATA     0
#define STEAMNETMSG_OFS_SIZE     4
#define STEAMNETMSG_OFS_IDENTITY 12

#if defined(__cplusplus)
#define BTW_SFLAT_ASSERT static_assert
#else
#define BTW_SFLAT_ASSERT _Static_assert
#endif
#if defined(__i386__) || defined(_M_IX86)
BTW_SFLAT_ASSERT(sizeof(CallbackMsg_t) == 16, "CallbackMsg_t");
BTW_SFLAT_ASSERT(sizeof(SteamAPICallCompleted_t) == 16, "SteamAPICallCompleted_t");
BTW_SFLAT_ASSERT(offsetof(LobbyCreated_t, m_ulSteamIDLobby) == 8, "LobbyCreated_t");
BTW_SFLAT_ASSERT(sizeof(LobbyEnter_t) == 24 && offsetof(LobbyEnter_t, m_EChatRoomEnterResponse) == 16, "LobbyEnter_t");
BTW_SFLAT_ASSERT(sizeof(LobbyDataUpdate_t) == 24, "LobbyDataUpdate_t");
BTW_SFLAT_ASSERT(sizeof(LobbyChatUpdate_t) == 32, "LobbyChatUpdate_t");
BTW_SFLAT_ASSERT(sizeof(GameLobbyJoinRequested_t) == 16, "GameLobbyJoinRequested_t");
BTW_SFLAT_ASSERT(sizeof(SteamNetworkingIdentity) == 136, "SteamNetworkingIdentity");
BTW_SFLAT_ASSERT(offsetof(SteamNetworkingMessage_t, m_identityPeer) == 12, "msg identity");
BTW_SFLAT_ASSERT(offsetof(SteamNetworkingMessage_t, m_nConnUserData) == 152, "msg conn user data");
BTW_SFLAT_ASSERT(offsetof(SteamNetworkingMessage_t, m_pfnRelease) == 180, "msg pfnRelease");
BTW_SFLAT_ASSERT(sizeof(SteamNetworkingMessage_t) == 208, "msg size");
#endif

/* ---- the exports: X(return type, export name, (parameters)) ------------------------- */
#define STEAM_FLAT_FUNCS(X) \
    X(int,            SteamAPI_InitFlat, (char *pOutErrMsg /* char[1024] */)) \
    X(void,           SteamAPI_Shutdown, (void)) \
    X(bool,           SteamAPI_IsSteamRunning, (void)) \
    X(HSteamPipe,     SteamAPI_GetHSteamPipe, (void)) \
    X(void,           SteamAPI_ManualDispatch_Init, (void)) \
    X(void,           SteamAPI_ManualDispatch_RunFrame, (HSteamPipe hSteamPipe)) \
    X(bool,           SteamAPI_ManualDispatch_GetNextCallback, (HSteamPipe hSteamPipe, CallbackMsg_t *pCallbackMsg)) \
    X(void,           SteamAPI_ManualDispatch_FreeLastCallback, (HSteamPipe hSteamPipe)) \
    X(bool,           SteamAPI_ManualDispatch_GetAPICallResult, (HSteamPipe hSteamPipe, SteamAPICall_t hSteamAPICall, void *pCallback, int cubCallback, int iCallbackExpected, bool *pbFailed)) \
    X(ISteamUser *,   SteamAPI_SteamUser_v023, (void)) \
    X(CSteamID_u64,   SteamAPI_ISteamUser_GetSteamID, (ISteamUser *self)) \
    X(ISteamMatchmaking *, SteamAPI_SteamMatchmaking_v009, (void)) \
    X(SteamAPICall_t, SteamAPI_ISteamMatchmaking_CreateLobby, (ISteamMatchmaking *self, int eLobbyType, int cMaxMembers)) \
    X(SteamAPICall_t, SteamAPI_ISteamMatchmaking_JoinLobby, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby)) \
    X(void,           SteamAPI_ISteamMatchmaking_LeaveLobby, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby)) \
    X(bool,           SteamAPI_ISteamMatchmaking_SetLobbyData, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby, const char *pchKey, const char *pchValue)) \
    X(const char *,   SteamAPI_ISteamMatchmaking_GetLobbyData, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby, const char *pchKey)) \
    X(bool,           SteamAPI_ISteamMatchmaking_SetLobbyJoinable, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby, bool bLobbyJoinable)) \
    X(CSteamID_u64,   SteamAPI_ISteamMatchmaking_GetLobbyOwner, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby)) \
    X(int,            SteamAPI_ISteamMatchmaking_GetNumLobbyMembers, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby)) \
    X(CSteamID_u64,   SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby, int iMember)) \
    X(bool,           SteamAPI_ISteamMatchmaking_InviteUserToLobby, (ISteamMatchmaking *self, CSteamID_u64 steamIDLobby, CSteamID_u64 steamIDInvitee)) \
    X(ISteamFriends *, SteamAPI_SteamFriends_v018, (void)) \
    X(bool,           SteamAPI_ISteamFriends_SetRichPresence, (ISteamFriends *self, const char *pchKey, const char *pchValue)) \
    X(void,           SteamAPI_ISteamFriends_ClearRichPresence, (ISteamFriends *self)) \
    X(void,           SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog, (ISteamFriends *self, CSteamID_u64 steamIDLobby)) \
    X(const char *,   SteamAPI_ISteamFriends_GetPersonaName, (ISteamFriends *self)) /* UTF-8 */ \
    X(ISteamNetworkingMessages *, SteamAPI_SteamNetworkingMessages_SteamAPI_v002, (void)) \
    X(EResult,        SteamAPI_ISteamNetworkingMessages_SendMessageToUser, (ISteamNetworkingMessages *self, const SteamNetworkingIdentity *identityRemote, const void *pubData, uint32_t cubData, int nSendFlags, int nRemoteChannel)) \
    X(int,            SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel, (ISteamNetworkingMessages *self, int nLocalChannel, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages)) \
    X(bool,           SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser, (ISteamNetworkingMessages *self, const SteamNetworkingIdentity *identityRemote)) \
    X(bool,           SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser, (ISteamNetworkingMessages *self, const SteamNetworkingIdentity *identityRemote)) \
    X(void,           SteamAPI_SteamNetworkingMessage_t_Release, (SteamNetworkingMessage_t *self)) \
    X(void,           SteamAPI_SteamNetworkingIdentity_SetSteamID64, (SteamNetworkingIdentity *self, uint64_t steamID)) \
    X(uint64_t,       SteamAPI_SteamNetworkingIdentity_GetSteamID64, (SteamNetworkingIdentity *self)) \
    X(ISteamNetworkingUtils *, SteamAPI_SteamNetworkingUtils_SteamAPI_v004, (void)) \
    X(bool,           SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32, (ISteamNetworkingUtils *self, int eValue, int32_t val)) \
    X(int64_t,        SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp, (ISteamNetworkingUtils *self)) /* us, same clock as m_usecTimeReceived */

/* function pointer types: PFN_<export name> */
#define STEAM_FLAT_PFN_(r, n, p) typedef r (__cdecl *PFN_##n) p;
STEAM_FLAT_FUNCS(STEAM_FLAT_PFN_)
#undef STEAM_FLAT_PFN_

#ifdef STEAM_FLAT_EXPORT
#define STEAM_FLAT_PROTO_(r, n, p) __declspec(dllexport) r __cdecl n p;
STEAM_FLAT_FUNCS(STEAM_FLAT_PROTO_)
#undef STEAM_FLAT_PROTO_
#endif

#ifdef __cplusplus
}
#endif
#endif
