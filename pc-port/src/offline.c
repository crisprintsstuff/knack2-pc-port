// SPDX-License-Identifier: GPL-2.0-or-later
// Offline behaviour for online and system services that no other file provides: PSN (auth,
// commerce, WebApi2, Toolkit2, session signaling, game intents), voice
// chat, sharing and streaming, crash dumps, sockets and a few system queries. Each import either
// succeeds as a no-op or fails as on a console that is signed out with no network. The first call
// of each one is logged as "OFFLINE <api>", so a run shows which of them the game touches.
// SIGNED_OUT and NET_DOWN are SDK values; UNAVAILABLE stands in where the exact code is unknown.
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGNED_OUT ((int32_t)UINT32_C(0x80550006))  // SCE_NP_ERROR_SIGNED_OUT
#define NET_DOWN ((int32_t)UINT32_C(0x80410132))    // SCE_NET_ERROR_ENETDOWN
#define UNAVAILABLE ((int32_t)UINT32_C(0x80020016)) // approximate: feature not available

static void note(volatile LONG *seen, const char *api) {
    if (!InterlockedExchange(seen, 1)) fprintf(stderr, "OFFLINE %s\n", api);
}

// ---- system ------------------------------------------------------------------------------------
static int32_t GUEST_ABI open_ps_id(uint8_t *id) {
    if (!id) return UNAVAILABLE;
    for (int i = 0; i < 16; ++i) id[i] = (uint8_t)(0xa0 + i); // fixed: no real console identity
    return 0;
}
static int32_t GUEST_ABI user_name(int32_t user, char *name, uint64_t size) {
    if (user < 0 || !name || !size) return (int32_t)UINT32_C(0x80960002); // SCE_USER_SERVICE_ERROR_INVALID_ARGUMENT
    const char *chosen = getenv("ISS_USER_NAME");
    snprintf(name, (size_t)size, "%s", chosen && *chosen ? chosen : "Player");
    return 0;
}
// SceSystemServiceHdrToneMapLuminance: max full-frame, max and min luminance in nits.
static int32_t GUEST_ABI hdr_luminance(float *out) {
    if (!out) return UNAVAILABLE;
    out[0] = 1000.0f; out[1] = 1000.0f; out[2] = 0.01f;
    return 0;
}
static int32_t GUEST_ABI privacy_setting(int32_t type, int32_t *value) { (void)type; if (!value) return UNAVAILABLE; *value = 0; return 0; }
static int32_t GUEST_ABI app_param_int(int32_t id, int32_t *value) { (void)id; if (!value) return UNAVAILABLE; *value = 0; return 0; }

// ---- error dialog: logs the code and reports itself finished ------------------------------------
static volatile LONG error_dialog_status;
static int32_t GUEST_ABI error_dialog_initialize(void) { InterlockedExchange(&error_dialog_status, 1); return 0; }
// SceErrorDialogParam: int32 size, int32 errorCode, userId, reserved.
static int32_t GUEST_ABI error_dialog_open(const int32_t *param) {
    if (!param) return UNAVAILABLE;
    fprintf(stderr, "ERROR_DIALOG code=0x%08x\n", (unsigned)param[1]);
    InterlockedExchange(&error_dialog_status, 3);
    return 0;
}
static int32_t GUEST_ABI error_dialog_status_now(void) { return InterlockedCompareExchange(&error_dialog_status, 0, 0); }
static int32_t GUEST_ABI error_dialog_terminate(void) { InterlockedExchange(&error_dialog_status, 0); return 0; }

// ---- network -----------------------------------------------------------------------------------
static volatile LONG next_net_id;
static int32_t GUEST_ABI net_new_id(void) { return (int32_t)InterlockedIncrement(&next_net_id); }
// No socket ever exists, so a wait only ever times out.
static int32_t GUEST_ABI epoll_wait(int32_t id, void *events, int32_t max, int32_t timeout_us) {
    (void)id; (void)events; (void)max;
    Sleep(timeout_us < 0 ? INFINITE : (DWORD)(timeout_us / 1000));
    return 0;
}
static const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0x01}; // locally administered
static int32_t GUEST_ABI mac_address(uint8_t *out, int32_t flags) { (void)flags; if (!out) return NET_DOWN; memcpy(out, mac, 6); return 0; }
static int32_t GUEST_ABI ether_ntostr(const uint8_t *a, char *out, uint64_t size) {
    if (!a || !out) return NET_DOWN;
    snprintf(out, (size_t)size, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
    return 0;
}

// ---- imports that return a constant ------------------------------------------------------------
// X(library, api, value). Arguments are ignored; a failure leaves the outputs untouched.
#define OFFLINE_CONSTANTS(X) \
    /* a PS4, not a PS5 */ \
    X("libkernel_cpumode_platform", sceKernelIsProspero, 0) \
    X("libSceSystemService", sceSystemServiceEnableSuspendConfirmationDialog, 0) \
    X("libSceSystemService", sceSystemServiceDisableSuspendConfirmationDialog, 0) \
    X("libSceSystemService", sceSystemServiceReportAbnormalTermination, 0) \
    X("libScePad", scePadResetLightBar, 0) \
    X("libSceSysmodule", sceSysmoduleUnloadModule, 0) \
    X("libSceAudioOut", sceAudioOutMasteringInit, 0) \
    X("libSceAudioOut", sceAudioOutMasteringSetParam, 0) \
    X("libSceCoredump", sceCoredumpRegisterCoredumpHandler, 0) \
    X("libSceCoredump", sceCoredumpSetUserDataType, 0) \
    X("libSceCoredump", sceCoredumpWriteUserData, 0) \
    X("libSceCoredump", sceCoredumpAttachMemoryRegionAsUserFile, 0) \
    X("libSceCoredump", sceCoredumpGetStopInfoCpu, UNAVAILABLE) \
    X("libSceCoredump", sceCoredumpGetStopInfoGpu, UNAVAILABLE) \
    X("libSceGameLiveStreaming", sceGameLiveStreamingInitialize, 0) \
    X("libSceShareUtility", sceShareUtilityInitializeEx2, 0) \
    X("libSceShareUtility", sceShareUtilityAdvanceInitialize, 0) \
    X("libSceShareUtility", sceShareUtilityAdvanceSetUploadContentData, 0) \
    X("libSceShareUtility", sceShareUtilityOpenShareMenuDefault, UNAVAILABLE) \
    X("libSceVoiceQoS", sceVoiceQoSInit, 0) \
    X("libSceVoiceQoS", sceVoiceQoSEnd, 0) \
    X("libSceVoiceQoS", sceVoiceQoSCreateLocalEndpoint, UNAVAILABLE) \
    X("libSceVoiceQoS", sceVoiceQoSCreateRemoteEndpoint, UNAVAILABLE) \
    X("libSceVoiceQoS", sceVoiceQoSDeleteLocalEndpoint, 0) \
    X("libSceVoiceQoS", sceVoiceQoSDeleteRemoteEndpoint, 0) \
    X("libSceVoiceQoS", sceVoiceQoSConnect, UNAVAILABLE) \
    X("libSceVoiceQoS", sceVoiceQoSDisconnect, 0) \
    X("libSceVoiceQoS", sceVoiceQoSReadPacket, UNAVAILABLE) \
    X("libSceVoiceQoS", sceVoiceQoSWritePacket, UNAVAILABLE) \
    X("libSceVoiceQoS", sceVoiceQoSSetLocalEndpointAttribute, UNAVAILABLE) \
    X("libSceVoiceQoS", sceVoiceQoSSetRemoteEndpointAttribute, UNAVAILABLE) \
    /* Network: initialized, but no interface is up. */ \
    X("libSceNetCtl", sceNetCtlCheckCallback, 0) \
    X("libSceNet", sceNetEpollControl, 0) \
    X("libSceNet", sceNetEpollDestroy, 0) \
    X("libSceNet", sceNetResolverDestroy, 0) \
    X("libSceNet", sceNetResolverStartAton, NET_DOWN) \
    X("libSceNet", sceNetResolverStartNtoa, NET_DOWN) \
    X("libSceNet", sceNetSocket, NET_DOWN) \
    X("libSceNet", sceNetSocketClose, NET_DOWN) \
    X("libSceNet", sceNetAccept, NET_DOWN) \
    X("libSceNet", sceNetBind, NET_DOWN) \
    X("libSceNet", sceNetConnect, NET_DOWN) \
    X("libSceNet", sceNetListen, NET_DOWN) \
    X("libSceNet", sceNetRecv, NET_DOWN) \
    X("libSceNet", sceNetRecvfrom, NET_DOWN) \
    X("libSceNet", sceNetSend, NET_DOWN) \
    X("libSceNet", sceNetSendto, NET_DOWN) \
    X("libSceNet", sceNetShutdown, NET_DOWN) \
    X("libSceNet", sceNetGetpeername, NET_DOWN) \
    X("libSceNet", sceNetGetsockname, NET_DOWN) \
    X("libSceNet", sceNetGetsockopt, NET_DOWN) \
    X("libSceNet", sceNetSetsockopt, NET_DOWN) \
    X("libSceHttp", sceHttpParseStatusLine, UNAVAILABLE) \
    X("libSceHttp2", sceHttp2Init, 1) \
    X("libSceSsl", sceSslGetCaCerts, UNAVAILABLE) \
    /* PSN. */ \
    X("libSceNpManager", sceNpCheckPlus, SIGNED_OUT) \
    X("libSceNpManager", sceNpGetAccountLanguage2, SIGNED_OUT) \
    X("libSceNpManager", sceNpGetParentalControlInfoA, SIGNED_OUT) \
    X("libSceNpManager", sceNpNotifyPlusFeature, 0) \
    X("libSceNpManager", sceNpRegisterNpReachabilityStateCallback, 0) \
    X("libSceNpAuth", sceNpAuthCreateRequest, 1) \
    X("libSceNpAuth", sceNpAuthDeleteRequest, 0) \
    X("libSceNpAuth", sceNpAuthGetAuthorizationCodeV3, SIGNED_OUT) \
    X("libSceNpAuth", sceNpAuthGetIdTokenV3, SIGNED_OUT) \
    X("libSceNpGameIntent", sceNpGameIntentInitialize, 0) \
    X("libSceNpGameIntent", sceNpGameIntentReceiveIntent, UNAVAILABLE) \
    X("libSceNpGameIntent", sceNpGameIntentGetPropertyValueString, UNAVAILABLE) \
    X("libSceNpProfileDialog", sceNpProfileDialogInitialize, 0) \
    X("libSceNpProfileDialog", sceNpProfileDialogOpenA, SIGNED_OUT) \
    /* initialized, never running */ \
    X("libSceNpProfileDialog", sceNpProfileDialogUpdateStatus, 1) \
    X("libSceNpProfileDialog", sceNpProfileDialogClose, 0) \
    X("libSceNpProfileDialog", sceNpProfileDialogTerminate, 0) \
    X("libSceNpCommerce", sceNpCommerceDialogInitialize, 0) \
    X("libSceNpCommerce", sceNpCommerceDialogOpen, SIGNED_OUT) \
    X("libSceNpCommerce", sceNpCommerceDialogUpdateStatus, 1) \
    X("libSceNpCommerce", sceNpCommerceDialogGetResult, SIGNED_OUT) \
    X("libSceNpCommerce", sceNpCommerceDialogTerminate, 0) \
    X("libSceNpCommerce", sceNpCommerceShowPsStoreIcon, 0) \
    X("libSceNpCommerce", sceNpCommerceHidePsStoreIcon, 0) \
    X("libSceNpCommerce", sceNpCommerceSetPsStoreIconLayout, 0) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingInitialize, 0) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingDeactivate, 0) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingActivateUser, SIGNED_OUT) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingCreateContext, SIGNED_OUT) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingRequestPrepare, SIGNED_OUT) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingGetLocalNetInfo, SIGNED_OUT) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingGetConnectionInfo, SIGNED_OUT) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingGetConnectionStatus, SIGNED_OUT) \
    X("libSceNpSessionSignaling", sceNpSessionSignalingGetConnectionFromPeerAddress, SIGNED_OUT) \
    /* WebApi2: the library context and push-event handles exist; anything per user fails. */ \
    X("libSceNpWebApi2", sceNpWebApi2Initialize, 1) \
    X("libSceNpWebApi2", sceNpWebApi2Terminate, 0) \
    X("libSceNpWebApi2", sceNpWebApi2CreateUserContext, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2DeleteUserContext, 0) \
    X("libSceNpWebApi2", sceNpWebApi2CreateRequest, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2DeleteRequest, 0) \
    X("libSceNpWebApi2", sceNpWebApi2AbortRequest, 0) \
    X("libSceNpWebApi2", sceNpWebApi2AddHttpRequestHeader, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2SendRequest, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2ReadData, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2GetHttpResponseHeaderValue, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventCreateFilter, 1) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventDeleteFilter, 0) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventCreateHandle, 1) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventDeleteHandle, 0) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventRegisterCallback, 1) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventUnregisterCallback, 0) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventCreatePushContext, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventDeletePushContext, 0) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventRegisterPushContextCallback, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventStartPushContextCallback, SIGNED_OUT) \
    X("libSceNpWebApi2", sceNpWebApi2PushEventUnregisterPushContextCallback, 0) \
    /* Toolkit2 (C++): initializes, every request then fails as signed out. Constructors and */ \
    /* destructors do nothing because the object layouts are unknown. */ \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core4initERKNS3_7Request10InitParamsE, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core7Request10InitParamsC1Ev, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core7Request10InitParamsD1Ev, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core8ResponseINS3_5EmptyEEC1Ev, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core8ResponseINS3_5EmptyEE5resetEv, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core12ResponseBase13getReturnCodeEv, SIGNED_OUT) \
    /* no server error object */ \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core12ResponseBase14getServerErrorEv, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core12ResponseBase8isLockedEv, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core15StringifyResultC1Ev, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core15StringifyResultD1Ev, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core22getServiceTypeAsStringENS3_11ServiceTypeERNS3_15StringifyResultE, UNAVAILABLE) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V24Core23getFunctionTypeAsStringENS3_12FunctionTypeERNS3_15StringifyResultE, UNAVAILABLE) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V212ActivityFeed7Request14PostPlayedWithC1Ev, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V212ActivityFeed7Request14PostPlayedWithD1Ev, 0) \
    X("libSceNpToolkit2", _ZN3sce7Toolkit2NP2V212ActivityFeed14postPlayedWithERKNS3_7Request14PostPlayedWithEPNS2_4Core8ResponseINS8_5EmptyEEE, SIGNED_OUT) \
    /* end */
#define DEFINE(library, api, value) \
    static int64_t GUEST_ABI offline_##api(void) { static volatile LONG seen; note(&seen, #api); return (int64_t)(value); }
OFFLINE_CONSTANTS(DEFINE)
#undef DEFINE

// ---- resolution ----------------------------------------------------------------------------------
static const struct { const char *library, *api; const void *fn; } table[] = {
#define ENTRY(library, api, value) {library, #api, (const void *)offline_##api},
    OFFLINE_CONSTANTS(ENTRY)
#undef ENTRY
    {"libSceOpenPsId", "sceKernelGetOpenPsId", (const void *)open_ps_id},
    {"libSceUserService", "sceUserServiceGetUserName", (const void *)user_name},
    {"libSceSystemService", "sceSystemServiceGetHdrToneMapLuminance", (const void *)hdr_luminance},
    {"libSceSystemServicePlatformPrivacy", "sceSystemServiceGetPlatformPrivacySetting", (const void *)privacy_setting},
    {"libSceAppContent", "sceAppContentAppParamGetInt", (const void *)app_param_int},
    {"libSceErrorDialog", "sceErrorDialogInitialize", (const void *)error_dialog_initialize},
    {"libSceErrorDialog", "sceErrorDialogOpen", (const void *)error_dialog_open},
    {"libSceErrorDialog", "sceErrorDialogUpdateStatus", (const void *)error_dialog_status_now},
    {"libSceErrorDialog", "sceErrorDialogTerminate", (const void *)error_dialog_terminate},
    {"libSceNet", "sceNetGetMacAddress", (const void *)mac_address},
    {"libSceNet", "sceNetEtherNtostr", (const void *)ether_ntostr},
    {"libSceNet", "sceNetEpollCreate", (const void *)net_new_id},
    {"libSceNet", "sceNetEpollWait", (const void *)epoll_wait},
    {"libSceNet", "sceNetResolverCreate", (const void *)net_new_id},
};
uintptr_t offline_resolve(const char *identity) {
    const char *api = runtime_api_name(identity);
    char prefix[96];
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i) {
        if (strcmp(api, table[i].api)) continue;
        snprintf(prefix, sizeof(prefix), "#%s#1#", table[i].library);
        if (strstr(identity, prefix)) return (uintptr_t)table[i].fn;
    }
    return 0;
}
