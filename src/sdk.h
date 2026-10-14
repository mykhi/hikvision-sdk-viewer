// Hikvision HCNetSDK, loaded at runtime from the user's SDK directory so the binary can be shipped without it.
#pragma once

#include <string>

#include "HCNetSDK.h"

#define HIK_SDK_FUNCTIONS(X)                                                                                  \
    X(NET_DVR_Init) X(NET_DVR_Cleanup) X(NET_DVR_SetSDKInitCfg) X(NET_DVR_SetConnectTime) X(NET_DVR_SetReconnect) \
    X(NET_DVR_GetLastError) X(NET_DVR_GetErrorMsg) X(NET_DVR_Login_V40) X(NET_DVR_Logout) X(NET_DVR_GetDVRConfig) \
    X(NET_DVR_RealPlay_V40) X(NET_DVR_StopRealPlay) X(NET_DVR_RenderPrivateData) X(NET_DVR_PlayBackByTime_V40)   \
    X(NET_DVR_PlayBackControl) X(NET_DVR_PlayBackControl_V40) X(NET_DVR_StopPlayBack)                          \
    X(NET_DVR_GetPlayBackOsdTime) X(NET_DVR_FindFile_V40) X(NET_DVR_FindNextFile_V40) X(NET_DVR_FindClose_V30)  \
    X(NET_DVR_GetFileByTime_V40) X(NET_DVR_GetDownloadPos) X(NET_DVR_StopGetFile) X(NET_DVR_CaptureJPEGPicture)

namespace sdk {

// Function pointers don't keep the header's default arguments: always pass every argument.
#define HIK_DECLARE(name) extern decltype(&::name) name;
HIK_SDK_FUNCTIONS(HIK_DECLARE)
#undef HIK_DECLARE

std::string dir();     // where `hikview setup` installs the SDK libraries
bool installed();
void load();           // dlopen + NET_DVR_Init; exits with a hint if the SDK is missing
std::string last_error();
[[noreturn]] void die(const char *what);

}  // namespace sdk
