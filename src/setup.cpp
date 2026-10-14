#include "setup.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "device.h"
#include "nvr.h"
#include "sdk.h"
#include "util.h"

static const char *SDK_PAGE = "https://www.hikvision.com/en/support/download/sdk/";

static std::string home() { return getenv("HOME") ? getenv("HOME") : ""; }
static std::string bin_path() { return home() + "/.local/bin/hikview"; }

static std::string apps_dir() {
    const char *data = getenv("XDG_DATA_HOME");
    return (data && *data ? std::string(data) : home() + "/.local/share") + "/applications";
}

static std::string desktop_path(const std::string &device) {
    return apps_dir() + (device == "default" ? "/hikview.desktop" : "/hikview-" + device + ".desktop");
}

static bool mkdirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/')
            if (mkdir(path.substr(0, i).c_str(), 0755) && errno != EEXIST) return false;
    return true;
}

static std::string self_path() {
    char exe[PATH_MAX] = {0};
    return readlink("/proc/self/exe", exe, sizeof(exe) - 1) > 0 ? exe : "";
}

static bool write_file(const std::string &path, const std::string &content, mode_t mode) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    bool ok = fchmod(fd, mode) == 0 && write(fd, content.data(), content.size()) == (ssize_t)content.size();
    ok = close(fd) == 0 && ok;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

static bool install_binary() {
    std::string self = self_path(), target = bin_path();
    if (self == target) return true;
    std::ifstream in(self, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty() || !mkdirs(home() + "/.local/bin") || !write_file(target, data, 0755)) {
        fprintf(stderr, "cannot install %s: %s\n", target.c_str(), strerror(errno));
        return false;
    }
    printf("installed %s\n", target.c_str());
    std::string path = std::string(":") + (getenv("PATH") ? getenv("PATH") : "") + ":";
    if (path.find(":" + home() + "/.local/bin:") == std::string::npos)
        printf("note: ~/.local/bin is not in your PATH yet - log out and back in, or run it as %s\n", target.c_str());
    return true;
}

static bool install_sdk(std::string zip) {
    if (zip.empty()) {
        printf("hikview needs Hikvision's Device Network SDK for Linux 64-bit, which it can't ship.\n"
               "Download \"Device Network SDK_Linux64\" (EN-HCNetSDK..._linux64.zip) from\n  %s\n", SDK_PAGE);
        zip = ask("path to the downloaded zip: ");
    }
    if (zip.rfind("~/", 0) == 0) zip = home() + zip.substr(1);
    if (zip.empty() || access(zip.c_str(), R_OK)) {
        fprintf(stderr, "cannot read SDK zip \"%s\"\n", zip.c_str());
        return false;
    }
    if (!have_command("unzip")) {
        fprintf(stderr, "unpacking the SDK needs unzip (apt install unzip)\n");
        return false;
    }
    char tmp[] = "/tmp/hikview-sdk-XXXXXX";
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return false; }
    std::string lib;
    if (run({"unzip", "-q", zip.c_str(), "-d", tmp}) == 0) {
        if (DIR *dir = opendir(tmp)) {
            while (dirent *e = readdir(dir)) {
                std::string candidate = std::string(tmp) + "/" + e->d_name + "/lib";
                if (e->d_name[0] != '.' && access((candidate + "/libhcnetsdk.so").c_str(), R_OK) == 0) lib = candidate;
            }
            closedir(dir);
        }
    }
    std::string dest = sdk::dir();
    bool ok = !lib.empty() && run({"rm", "-rf", dest.c_str()}) == 0 && mkdirs(dest) &&
              run({"cp", "-a", (lib + "/.").c_str(), dest.c_str()}) == 0;
    run({"rm", "-rf", tmp});
    if (lib.empty())
        fprintf(stderr, "%s doesn't look like the Linux 64-bit Device Network SDK (no lib/libhcnetsdk.so)\n", zip.c_str());
    else if (!ok)
        fprintf(stderr, "copying the SDK to %s failed\n", dest.c_str());
    else
        printf("installed the Hikvision SDK to %s\n", dest.c_str());
    return ok;
}

static bool configure_device(const std::string &name) {
    Device old;
    bool existing = load_device(device_path(name), old);
    printf("\ndevice \"%s\"%s\n", name.c_str(), existing ? " (press Enter to keep the current value)" : "");

    std::string address = ask("NVR address as host:port (the SDK port, usually 8000)" +
                              (existing ? " [" + old.address + "]" : std::string()) + ": ");
    if (address.empty()) address = old.address;
    if (address.empty()) { fprintf(stderr, "setup needs a terminal and an NVR address\n"); return false; }
    if (address.find(':') == std::string::npos) address += ":8000";
    std::string def_user = existing ? old.user : "admin";
    std::string user = ask("user [" + def_user + "]: ");
    if (user.empty()) user = def_user;
    std::string pass = ask("password" + std::string(existing ? " (Enter keeps the current one)" : "") + ": ", true);

    std::string file_pass;
    if (pass.empty() && existing) {
        file_pass = old.password;  // unchanged; keyring entry (if any) stays as it is
    } else if (pass.empty()) {
        printf("no password saved - hikview will ask for it each time (or set HIK_PASSWORD)\n");
    } else if (keyring_available() && keyring_store(address, user, pass)) {
        printf("password saved in the system keyring\n");
    } else {
        printf("password saved in the config file (mode 600)%s\n",
               keyring_available() ? "" : " - install libsecret-tools to use the system keyring instead");
        file_pass = pass;
    }
    explicit_bzero(&pass[0], pass.size());

    std::string content = address + "\n" + user + "\n" + (file_pass.empty() ? "" : file_pass + "\n");
    std::string path = device_path(name);
    bool ok = mkdirs(config_dir() + "/devices") && write_file(path, content, 0600);
    explicit_bzero(&content[0], content.size());
    if (!ok) { fprintf(stderr, "cannot write %s: %s\n", path.c_str(), strerror(errno)); return false; }
    printf("saved %s\n", path.c_str());
    return true;
}

static void install_menu_entry(const std::string &device) {
    std::string exec = "\"" + bin_path() + "\"" + (device == "default" ? "" : " -d " + device);
    std::string entry = "[Desktop Entry]\n"
                        "Type=Application\n"
                        "Name=Hikvision cameras" + (device == "default" ? "" : " - " + device) + "\n"
                        "Comment=Live view of Hikvision NVR cameras\n"
                        "Exec=" + exec + " grid\n"
                        "Icon=camera-web\n"
                        "Terminal=false\n"
                        "Categories=AudioVideo;Video;\n"
                        "StartupWMClass=hikview\n";
    std::string path = desktop_path(device);
    if (mkdirs(apps_dir()) && write_file(path, entry, 0644))
        printf("added menu entry %s\n", path.c_str());
    else
        fprintf(stderr, "cannot write menu entry %s\n", path.c_str());
}

int cmd_setup(const std::string &device, const char *sdk_zip) {
    if (!install_binary()) return 1;
    if (sdk_zip || !sdk::installed()) {
        if (!install_sdk(sdk_zip ? sdk_zip : "")) return 1;
    } else {
        printf("Hikvision SDK already installed in %s (hikview setup --sdk ZIP replaces it)\n", sdk::dir().c_str());
    }
    if (!configure_device(device)) return 1;
    install_menu_entry(device);

    printf("\nchecking the connection...\n");
    Device d;
    load_device(device_path(device), d);
    sdk::load();
    NET_DVR_DEVICEINFO_V40 info = {};
    LONG uid = login(d, &info);
    cmd_list(uid, info);
    sdk::NET_DVR_Logout(uid);
    sdk::NET_DVR_Cleanup();
    printf("\ndone - start it from the app menu (\"Hikvision cameras\") or run: hikview%s\n",
           device == "default" ? "" : (" -d " + device).c_str());
    return 0;
}

static void remove_path(const std::string &path, bool recursive = false) {
    if (access(path.c_str(), F_OK)) return;
    if (recursive ? run({"rm", "-rf", path.c_str()}) == 0 : unlink(path.c_str()) == 0)
        printf("removed %s\n", path.c_str());
    else
        fprintf(stderr, "cannot remove %s\n", path.c_str());
}

int cmd_uninstall(bool purge) {
    if (purge) {
        for (const std::string &name : device_names()) {
            Device d;
            if (load_device(device_path(name), d) && keyring_available()) keyring_clear(d.address, d.user);
        }
        remove_path(config_dir(), true);
    }
    if (DIR *dir = opendir(apps_dir().c_str())) {
        while (dirent *e = readdir(dir)) {
            std::string n = e->d_name;
            if (n == "hikview.desktop" || (n.rfind("hikview-", 0) == 0 && n.size() > 8 &&
                                           n.compare(n.size() - 8, 8, ".desktop") == 0))
                remove_path(apps_dir() + "/" + n);
        }
        closedir(dir);
    }
    remove_path(sdk::dir(), true);
    std::string sdk_parent = sdk::dir().substr(0, sdk::dir().rfind('/'));
    rmdir(sdk_parent.c_str());
    remove_path(bin_path());
    if (!purge) printf("kept device logins in %s (hikview uninstall --purge removes them)\n", config_dir().c_str());
    return 0;
}
