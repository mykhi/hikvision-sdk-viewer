#include "sdk.h"

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>

namespace sdk {

#define HIK_DEFINE(name) decltype(&::name) name = nullptr;
HIK_SDK_FUNCTIONS(HIK_DEFINE)
#undef HIK_DEFINE

std::string dir() {
    const char *data = getenv("XDG_DATA_HOME");
    std::string base = data && *data ? data : std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/share";
    return base + "/hikview/sdk";
}

bool installed() { return access((dir() + "/libhcnetsdk.so").c_str(), R_OK) == 0; }

void load() {
    std::string d = dir();
    // libhcnetsdk.so has RUNPATH $ORIGIN, so its own dependencies resolve from the same directory.
    void *h = dlopen((d + "/libhcnetsdk.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "Hikvision SDK not found (%s)\nrun: hikview setup --sdk <EN-HCNetSDK..._linux64.zip>\n", dlerror());
        exit(1);
    }
#define HIK_RESOLVE(name)                                                                             \
    name = (decltype(name))dlsym(h, #name);                                                            \
    if (!name) {                                                                                       \
        fprintf(stderr, "the Hikvision SDK in %s has no %s - install a newer one with hikview setup\n", \
                d.c_str(), #name);                                                                     \
        exit(1);                                                                                       \
    }
    HIK_SDK_FUNCTIONS(HIK_RESOLVE)
#undef HIK_RESOLVE

    NET_DVR_LOCAL_SDK_PATH sdk_path = {};
    snprintf(sdk_path.sPath, sizeof(sdk_path.sPath), "%s/", d.c_str());
    NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SDK_PATH, &sdk_path);
    std::string crypto = d + "/libcrypto.so.3", ssl = d + "/libssl.so.3";
    NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_LIBEAY_PATH, (void *)crypto.c_str());
    NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SSLEAY_PATH, (void *)ssl.c_str());

    if (!NET_DVR_Init()) die("NET_DVR_Init");
    NET_DVR_SetConnectTime(5000, 1);
    NET_DVR_SetReconnect(10000, TRUE);
}

std::string last_error() {
    LONG err = NET_DVR_GetLastError();
    char buf[256];
    snprintf(buf, sizeof(buf), "error %d (%s)", err, NET_DVR_GetErrorMsg(&err));
    return buf;
}

void die(const char *what) {
    fprintf(stderr, "%s failed: %s\n", what, last_error().c_str());
    NET_DVR_Cleanup();
    exit(1);
}

}  // namespace sdk
