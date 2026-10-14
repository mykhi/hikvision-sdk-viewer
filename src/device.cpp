#include "device.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

#include "util.h"

std::string config_dir() {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    std::string base = cfg && *cfg ? cfg : std::string(getenv("HOME") ? getenv("HOME") : "") + "/.config";
    return base + "/hikview";
}

std::string device_path(const std::string &name) { return config_dir() + "/devices/" + name; }

std::vector<std::string> device_names() {
    std::vector<std::string> names;
    if (DIR *dir = opendir((config_dir() + "/devices").c_str())) {
        while (dirent *e = readdir(dir))
            if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool load_device(const std::string &path, Device &d) {
    std::ifstream f(path);
    if (!std::getline(f, d.address) || !std::getline(f, d.user)) return false;
    std::getline(f, d.password);
    for (std::string *s : {&d.address, &d.user, &d.password})
        while (!s->empty() && (s->back() == '\r' || s->back() == ' ')) s->pop_back();
    d.path = path;

    struct stat st;
    if (!d.password.empty() && stat(path.c_str(), &st) == 0 && (st.st_mode & 077)) {
        fprintf(stderr, "%s contains a password but other users can read it; run: chmod 600 %s\n", path.c_str(),
                path.c_str());
        exit(1);
    }
    return !d.address.empty() && !d.user.empty();
}

LONG login(Device &d, NET_DVR_DEVICEINFO_V40 *info) {
    std::string pass = d.password;
    if (pass.empty() && getenv("HIK_PASSWORD")) pass = getenv("HIK_PASSWORD");
    if (pass.empty())
        pass = run_line({"secret-tool", "lookup", "service", "hikview", "address", d.address.c_str(), "user",
                         d.user.c_str()});
    if (pass.empty()) pass = ask("password for " + d.user + "@" + d.address + ": ", true);
    if (pass.empty()) {
        fprintf(stderr, "no password for %s@%s: run 'hikview setup', set HIK_PASSWORD, or start from a terminal\n",
                d.user.c_str(), d.address.c_str());
        exit(1);
    }

    size_t colon = d.address.rfind(':');
    std::string host = colon == std::string::npos ? d.address : d.address.substr(0, colon);
    int port = colon == std::string::npos ? 8000 : atoi(d.address.c_str() + colon + 1);

    NET_DVR_USER_LOGIN_INFO li = {};
    snprintf(li.sDeviceAddress, sizeof(li.sDeviceAddress), "%s", host.c_str());
    snprintf(li.sUserName, sizeof(li.sUserName), "%s", d.user.c_str());
    snprintf(li.sPassword, sizeof(li.sPassword), "%s", pass.c_str());
    li.wPort = (WORD)port;
    li.bUseAsynLogin = FALSE;

    fprintf(stderr, "logging in to %s:%d as %s ...\n", host.c_str(), port, d.user.c_str());
    LONG uid = sdk::NET_DVR_Login_V40(&li, info);
    explicit_bzero(li.sPassword, sizeof(li.sPassword));
    explicit_bzero(&pass[0], pass.size());
    if (uid < 0 && sdk::NET_DVR_GetLastError() == NET_DVR_PASSWORD_ERROR) {
        if (info->bySupportLock)
            fprintf(stderr, "wrong user or password - %d attempt(s) left before the device locks this account\n",
                    info->byRetryLoginTime);
        else
            fprintf(stderr, "wrong user or password - the device locks the account after several failed attempts\n");
    }
    if (uid < 0) sdk::die("NET_DVR_Login_V40");
    return uid;
}

bool keyring_available() { return have_command("secret-tool"); }

bool keyring_store(const std::string &address, const std::string &user, const std::string &password) {
    std::string label = "hikview " + user + "@" + address;
    return run({"secret-tool", "store", "--label", label.c_str(), "service", "hikview", "address", address.c_str(),
                "user", user.c_str()},
               &password) == 0;
}

void keyring_clear(const std::string &address, const std::string &user) {
    run_line({"secret-tool", "clear", "service", "hikview", "address", address.c_str(), "user", user.c_str()});
}
