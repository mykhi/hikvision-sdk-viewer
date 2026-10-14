// hikview - live view, playback and export for Hikvision NVRs on Linux via the Hikvision SDK.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "device.h"
#include "nvr.h"
#include "sdk.h"
#include "setup.h"
#include "util.h"
#include "view.h"

#ifndef HIKVIEW_VERSION
#define HIKVIEW_VERSION "dev"
#endif

static const char *USAGE = R"(usage: hikview [-d NAME | -c FILE] [command]

commands:
  (none), grid [CH...]      live view of all online cameras (or the given channels)
  live CH                   one camera, full quality
  play CH TIME              play back recordings (keys are printed on start)
  recordings CH [DATE]      recorded time ranges of a day (default today)
  export CH FROM TO FILE    save a recording as .mp4 (needs ffmpeg) or raw .ps
  snap CH FILE.jpg          snapshot taken by the NVR
  list                      device info and channels
  stream CH [sub]           raw MPEG-PS to stdout, e.g. | ffplay -f mpeg -i -
  setup [--sdk ZIP]         install hikview, the Hikvision SDK, a device login and a menu entry
  uninstall [--purge]       remove hikview, the SDK and menu entries (--purge: also device logins)

options:
  -d NAME        device to use (default: "default"); add one with: hikview -d NAME setup
  -c FILE        device config file to use instead
  -V, --version

TIME: "2026-10-13 21:00[:SS]", "21:00" (today), "2026-10-13", or relative "-90s" "-10m" "-2h" "-1d"
)";

static void on_signal(int) { g_stop = 1; }

[[noreturn]] static void usage_error(const char *msg) {
    fprintf(stderr, "%s\n\n%s", msg, USAGE);
    exit(2);
}

static int channel_arg(const char *s) {
    char *end;
    long ch = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || ch <= 0 || ch > 9999) usage_error(("invalid channel \"" + std::string(s) + "\"").c_str());
    return (int)ch;
}

static bool valid_device_name(const std::string &n) {
    if (n.empty() || n[0] == '.' || n.size() > 64) return false;
    for (char c : n)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

int main(int argc, char **argv) {
    std::string device = "default";
    const char *config = nullptr;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        std::string opt = argv[i];
        if ((opt == "-d" || opt == "-c") && i + 1 >= argc) usage_error(("missing value for " + opt).c_str());
        if (opt == "-d") {
            device = argv[++i];
            if (!valid_device_name(device)) usage_error("device names may contain letters, digits, '-', '_' and '.'");
        } else if (opt == "-c") {
            config = argv[++i];
        } else if (opt == "-h" || opt == "--help") {
            printf("%s", USAGE);
            return 0;
        } else if (opt == "-V" || opt == "--version") {
            printf("hikview %s\n", HIKVIEW_VERSION);
            return 0;
        } else {
            usage_error(("unknown option " + opt).c_str());
        }
    }
    std::string cmd = i < argc ? argv[i++] : "grid";
    std::vector<const char *> args(argv + i, argv + argc);
    auto need = [&](size_t min, size_t max) {
        if (args.size() < min || args.size() > max) usage_error(("wrong number of arguments for " + cmd).c_str());
    };

    if (cmd == "help") { printf("%s", USAGE); return 0; }
    if (cmd == "setup") {
        need(0, 2);
        if (!args.empty() && (strcmp(args[0], "--sdk") || args.size() != 2)) usage_error("usage: hikview setup [--sdk ZIP]");
        return cmd_setup(device, args.empty() ? nullptr : args[1]);
    }
    if (cmd == "uninstall") {
        need(0, 1);
        if (!args.empty() && strcmp(args[0], "--purge")) usage_error("usage: hikview uninstall [--purge]");
        return cmd_uninstall(!args.empty());
    }

    // Validate arguments before connecting, so typos fail fast.
    time_t t1 = 0, t2 = 0;
    std::vector<int> channels;
    if (cmd == "grid") {
        for (const char *a : args) channels.push_back(channel_arg(a));
    } else if (cmd == "live") {
        need(1, 1);
    } else if (cmd == "play") {
        need(2, 2);
        t1 = parse_time(args[1]);
    } else if (cmd == "recordings") {
        need(1, 2);
        t1 = args.size() > 1 ? parse_time(args[1]) : time(nullptr);
    } else if (cmd == "export") {
        need(4, 4);
        t1 = parse_time(args[1]);
        t2 = parse_time(args[2]);
    } else if (cmd == "snap") {
        need(2, 2);
    } else if (cmd == "list") {
        need(0, 0);
    } else if (cmd == "stream") {
        need(1, 2);
        if (args.size() == 2 && strcmp(args[1], "sub")) usage_error("usage: hikview stream CH [sub]");
    } else {
        usage_error(("unknown command " + cmd).c_str());
    }
    int ch = cmd != "grid" && cmd != "list" ? channel_arg(args[0]) : 0;

    Device d;
    if (!load_device(config ? config : device_path(device), d)) {
        if (config)
            fprintf(stderr, "cannot read host:port and user from %s\n", config);
        else if (device_names().empty())
            fprintf(stderr, "hikview isn't set up yet - run: hikview setup\n");
        else
            fprintf(stderr, "no device \"%s\" - run: hikview -d %s setup\n", device.c_str(), device.c_str());
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    sdk::load();
    NET_DVR_DEVICEINFO_V40 info = {};
    LONG uid = login(d, &info);

    int rc = 0;
    if (cmd == "grid") cmd_grid(uid, channels);
    else if (cmd == "live") cmd_grid(uid, {ch});
    else if (cmd == "play") cmd_play(uid, ch, t1);
    else if (cmd == "recordings") cmd_recordings(uid, ch, t1);
    else if (cmd == "export") rc = cmd_export(uid, ch, t1, t2, args[3]);
    else if (cmd == "snap") rc = cmd_snap(uid, ch, args[1]);
    else if (cmd == "list") cmd_list(uid, info);
    else if (cmd == "stream") cmd_stream(uid, ch, args.size() == 2);

    sdk::NET_DVR_Logout(uid);
    sdk::NET_DVR_Cleanup();
    return rc;
}
