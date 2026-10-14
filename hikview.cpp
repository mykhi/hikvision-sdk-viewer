// Native Hikvision client on top of HCNetSDK.
//   hikview <config> list                    - log in, print device info and channels
//   hikview <config> stream <channel> [sub]  - write live MPEG-PS to stdout (pipe into ffplay)
//   hikview <config> grid [channel...]       - all online cameras in one window (click = zoom, f = fullscreen,
//                                              m = motion overlays, q = quit)
//   hikview <config> play <channel> <time>   - play back recordings in a window (keys printed on start)
//   hikview <config> recordings <channel> [date]          - list recorded segments of a day
//   hikview <config> export <channel> <from> <to> <file>  - download recording as Hikvision PS file
// <time>: "YYYY-MM-DD HH:MM[:SS]", "HH:MM[:SS]" (today), "YYYY-MM-DD", or relative "-30s" "-10m" "-2h" "-1d".
// <config> = text file: line 1 "host:port", line 2 user, optional line 3 password (file must be mode 600).
// Without line 3 the password comes from $HIK_PASSWORD, the system keyring (secret-tool), or a terminal prompt.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <ctime>
#include <vector>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <libgen.h>
#include <linux/limits.h>

#include "HCNetSDK.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

static long long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// NVR clock is local time (NTP-synced, same as this PC).
static NET_DVR_TIME to_dvr(time_t t) {
    struct tm lt;
    localtime_r(&t, &lt);
    NET_DVR_TIME d = {};
    d.dwYear = lt.tm_year + 1900, d.dwMonth = lt.tm_mon + 1, d.dwDay = lt.tm_mday;
    d.dwHour = lt.tm_hour, d.dwMinute = lt.tm_min, d.dwSecond = lt.tm_sec;
    return d;
}

static time_t from_dvr(const NET_DVR_TIME &d) {
    struct tm lt = {};
    lt.tm_year = d.dwYear - 1900, lt.tm_mon = d.dwMonth - 1, lt.tm_mday = d.dwDay;
    lt.tm_hour = d.dwHour, lt.tm_min = d.dwMinute, lt.tm_sec = d.dwSecond;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

static std::string fmt(time_t t, const char *f = "%Y-%m-%d %H:%M:%S") {
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[64];
    strftime(buf, sizeof(buf), f, &lt);
    return buf;
}

static time_t parse_time(const char *s) {
    time_t now = time(nullptr);
    if (s[0] == '-') {
        char *end;
        long v = strtol(s + 1, &end, 10);
        long unit = !strcmp(end, "s") ? 1 : !strcmp(end, "m") ? 60 : !strcmp(end, "h") ? 3600 : !strcmp(end, "d") ? 86400 : 0;
        if (end != s + 1 && unit) return now - v * unit;
    } else {
        struct tm today;
        localtime_r(&now, &today);
        today.tm_hour = today.tm_min = today.tm_sec = 0;
        for (const char *f : {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d", "%H:%M:%S", "%H:%M"}) {
            struct tm t = today;
            const char *end = strptime(s, f, &t);
            if (end && *end == '\0') {
                t.tm_isdst = -1;
                return mktime(&t);
            }
        }
    }
    fprintf(stderr, "cannot parse time \"%s\"\n", s);
    exit(2);
}

static void die_sdk(const char *what) {
    LONG err = NET_DVR_GetLastError();
    fprintf(stderr, "%s failed: error %d (%s)\n", what, err, NET_DVR_GetErrorMsg(&err));
    NET_DVR_Cleanup();
    exit(1);
}

static void init_sdk() {
    char exe[PATH_MAX] = {0};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) < 0) { perror("readlink"); exit(1); }
    std::string lib = std::string(dirname(exe)) + "/lib/";

    NET_DVR_LOCAL_SDK_PATH sdk_path = {};
    snprintf(sdk_path.sPath, sizeof(sdk_path.sPath), "%s", lib.c_str());
    NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SDK_PATH, &sdk_path);
    std::string crypto = lib + "libcrypto.so.3", ssl = lib + "libssl.so.3";
    NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_LIBEAY_PATH, (void *)crypto.c_str());
    NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SSLEAY_PATH, (void *)ssl.c_str());

    if (!NET_DVR_Init()) die_sdk("NET_DVR_Init");
    NET_DVR_SetConnectTime(5000, 1);
    NET_DVR_SetReconnect(10000, TRUE);
}

// Runs a command without a shell and returns the first line of its output ("" on any failure).
static std::string run_line(std::vector<const char *> argv) {
    int fd[2];
    if (pipe(fd)) return "";
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(fd[1], STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        close(fd[0]);
        close(fd[1]);
        argv.push_back(nullptr);
        execvp(argv[0], (char *const *)argv.data());
        _exit(127);
    }
    close(fd[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while (pid > 0 && (n = read(fd[0], buf, sizeof(buf))) > 0) out.append(buf, n);
    close(fd[0]);
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return "";
    return out.substr(0, out.find('\n'));
}

static std::string prompt_password(const std::string &who) {
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) return "";
    fprintf(tty, "password for %s: ", who.c_str());
    fflush(tty);
    termios old, quiet;
    tcgetattr(fileno(tty), &old);
    quiet = old;
    quiet.c_lflag &= ~ECHO;
    tcsetattr(fileno(tty), TCSAFLUSH, &quiet);
    char buf[128] = {0};
    bool ok = fgets(buf, sizeof(buf), tty) != nullptr;
    tcsetattr(fileno(tty), TCSAFLUSH, &old);
    fprintf(tty, "\n");
    fclose(tty);
    std::string pass = ok ? std::string(buf, strcspn(buf, "\r\n")) : "";
    explicit_bzero(buf, sizeof(buf));
    return pass;
}

static LONG login(const char *config, NET_DVR_DEVICEINFO_V40 *info) {
    std::ifstream f(config);
    std::string hostport, user, pass;
    if (!std::getline(f, hostport) || !std::getline(f, user)) {
        fprintf(stderr, "cannot read host:port / user from %s\n", config);
        exit(1);
    }
    std::getline(f, pass);
    for (std::string *s : {&hostport, &user, &pass})
        while (!s->empty() && (s->back() == '\r' || s->back() == ' ')) s->pop_back();

    struct stat st;
    if (!pass.empty() && stat(config, &st) == 0 && (st.st_mode & 077)) {
        fprintf(stderr, "%s contains a password but other users can read it; run: chmod 600 %s\n", config, config);
        exit(1);
    }
    if (pass.empty() && getenv("HIK_PASSWORD")) pass = getenv("HIK_PASSWORD");
    if (pass.empty())
        pass = run_line({"secret-tool", "lookup", "service", "hikview", "address", hostport.c_str(), "user", user.c_str()});
    if (pass.empty()) pass = prompt_password(user + "@" + hostport);
    if (pass.empty()) {
        fprintf(stderr, "no password for %s@%s: run 'hikview.sh setup', set HIK_PASSWORD, or start from a terminal\n",
                user.c_str(), hostport.c_str());
        exit(1);
    }

    size_t colon = hostport.rfind(':');
    std::string host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    int port = colon == std::string::npos ? 8000 : atoi(hostport.c_str() + colon + 1);

    NET_DVR_USER_LOGIN_INFO li = {};
    snprintf(li.sDeviceAddress, sizeof(li.sDeviceAddress), "%s", host.c_str());
    snprintf(li.sUserName, sizeof(li.sUserName), "%s", user.c_str());
    snprintf(li.sPassword, sizeof(li.sPassword), "%s", pass.c_str());
    li.wPort = (WORD)port;
    li.bUseAsynLogin = FALSE;

    fprintf(stderr, "logging in to %s:%d as %s ...\n", host.c_str(), port, user.c_str());
    LONG uid = NET_DVR_Login_V40(&li, info);
    explicit_bzero(li.sPassword, sizeof(li.sPassword));
    explicit_bzero(&pass[0], pass.size());
    if (uid < 0 && NET_DVR_GetLastError() == NET_DVR_PASSWORD_ERROR) {
        if (info->bySupportLock)
            fprintf(stderr, "wrong user or password - %d attempt(s) left before the device locks this account\n",
                    info->byRetryLoginTime);
        else
            fprintf(stderr, "wrong user or password - the device locks the account after several failed attempts\n");
    }
    if (uid < 0) die_sdk("NET_DVR_Login_V40");
    return uid;
}

static void cmd_list(LONG uid, const NET_DVR_DEVICEINFO_V40 &info) {
    const NET_DVR_DEVICEINFO_V30 &d = info.struDeviceV30;
    char serial[SERIALNO_LEN + 1] = {0};
    memcpy(serial, d.sSerialNumber, SERIALNO_LEN);
    int ip_chans = d.byIPChanNum + d.byHighDChanNum * 256;

    printf("serial:          %s\n", serial);
    printf("analog channels: %d (first = %d)\n", d.byChanNum, d.byStartChan);
    printf("IP channels:     %d (first = %d)\n", ip_chans, d.byStartDChan);

    NET_DVR_TIME t = {};
    DWORD n = 0;
    if (NET_DVR_GetDVRConfig(uid, NET_DVR_GET_TIMECFG, 0, &t, sizeof(t), &n)) {
        time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        printf("NVR clock:       %04u-%02u-%02u %02u:%02u:%02u  (this PC: %02d:%02d:%02d)\n", t.dwYear, t.dwMonth,
               t.dwDay, t.dwHour, t.dwMinute, t.dwSecond, lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
    NET_DVR_NTPPARA ntp = {};
    if (NET_DVR_GetDVRConfig(uid, NET_DVR_GET_NTPCFG, 0, &ntp, sizeof(ntp), &n))
        printf("NVR NTP:         %s (server %.64s, every %u h)\n", ntp.byEnableNTP ? "on" : "off",
               (const char *)ntp.sNTPServer, ntp.wInterval);

    for (int i = 0; i < d.byChanNum; i++)
        printf("  channel %3d  analog\n", d.byStartChan + i);

    if (ip_chans == 0) return;
    NET_DVR_IPPARACFG_V40 ip = {};
    DWORD ret = 0;
    if (!NET_DVR_GetDVRConfig(uid, NET_DVR_GET_IPPARACFG_V40, 0, &ip, sizeof(ip), &ret)) {
        LONG err = NET_DVR_GetLastError();
        fprintf(stderr, "IP channel config unavailable: error %d (%s)\n", err, NET_DVR_GetErrorMsg(&err));
        return;
    }
    for (DWORD i = 0; i < ip.dwDChanNum && i < MAX_CHANNUM_V30; i++) {
        const NET_DVR_STREAM_MODE &m = ip.struStreamMode[i];
        if (m.byGetStreamType != 0) continue;
        const NET_DVR_IPCHANINFO &c = m.uGetStream.struChanInfo;
        int dev = c.byIPID + c.byIPIDHigh * 256;
        if (dev == 0) continue;
        const NET_DVR_IPDEVINFO_V31 &di = ip.struIPDevInfo[dev - 1];
        static const char *timing[] = {"default", "off", "on"};
        printf("  channel %3u  IP camera %-15s  time sync from NVR: %-7s  %s\n", ip.dwStartDChan + i,
               di.struIP.sIpV4, di.byEnableTiming < 3 ? timing[di.byEnableTiming] : "?",
               c.byEnable ? "online" : "OFFLINE");
    }
}

static std::vector<int> online_channels(LONG uid) {
    std::vector<int> out;
    NET_DVR_IPPARACFG_V40 ip = {};
    DWORD ret = 0;
    if (!NET_DVR_GetDVRConfig(uid, NET_DVR_GET_IPPARACFG_V40, 0, &ip, sizeof(ip), &ret)) return out;
    for (DWORD i = 0; i < ip.dwDChanNum && i < MAX_CHANNUM_V30; i++) {
        const NET_DVR_STREAM_MODE &m = ip.struStreamMode[i];
        if (m.byGetStreamType == 0 && m.uGetStream.struChanInfo.byEnable) out.push_back(ip.dwStartDChan + i);
    }
    return out;
}

struct Cell {
    int channel;
    Window win;
    LONG handle = -1;
};

static bool g_overlays = false;

static bool set_overlays(LONG handle, bool on) {
    bool ok = true;
    for (int type : {RENDER_ANA_INTEL_DATA, RENDER_MD, RENDER_ADD_POS, RENDER_ADD_PIC, RENDER_FIRE_DETCET, RENDER_TEM})
        ok &= NET_DVR_RenderPrivateData(handle, type, on) != 0;
    return ok;
}

static void play(LONG uid, Cell &c, bool main_stream) {
    if (c.handle >= 0) NET_DVR_StopRealPlay(c.handle);
    NET_DVR_PREVIEWINFO pv = {};
    pv.lChannel = c.channel;
    pv.dwStreamType = main_stream ? 0 : 1;
    pv.hPlayWnd = (HWND)c.win;
    pv.bBlocked = 0;
    pv.dwDisplayBufNum = 1;  // render immediately instead of smoothing with a frame buffer
    c.handle = NET_DVR_RealPlay_V40(uid, &pv, nullptr, nullptr);
    if (c.handle < 0) {
        LONG err = NET_DVR_GetLastError();
        fprintf(stderr, "channel %d: RealPlay failed: error %d (%s)\n", c.channel, err, NET_DVR_GetErrorMsg(&err));
    } else if (!set_overlays(c.handle, g_overlays)) {
        LONG err = NET_DVR_GetLastError();
        fprintf(stderr, "channel %d: overlay setting failed: error %d (%s)\n", c.channel, err, NET_DVR_GetErrorMsg(&err));
    }
}

static void layout(Display *dpy, std::vector<Cell> &cells, int w, int h, int zoomed) {
    int n = cells.size(), cols = 1;
    while (cols * cols < n) cols++;
    int rows = (n + cols - 1) / cols;
    for (int i = 0; i < n; i++) {
        if (zoomed >= 0 && i != zoomed) { XUnmapWindow(dpy, cells[i].win); continue; }
        if (zoomed >= 0)
            XMoveResizeWindow(dpy, cells[i].win, 0, 0, w, h);
        else
            XMoveResizeWindow(dpy, cells[i].win, i % cols * w / cols, i / cols * h / rows, w / cols - 2, h / rows - 2);
        XMapWindow(dpy, cells[i].win);
    }
    XSync(dpy, False);
}

struct XWin {
    Display *dpy = nullptr;
    Window win = 0;
    Atom wm_delete = 0;
};

static bool open_window(const char *title, int w, int h, XWin &x) {
    XInitThreads();
    x.dpy = XOpenDisplay(nullptr);
    if (!x.dpy) { fprintf(stderr, "cannot open X display\n"); return false; }
    int scr = DefaultScreen(x.dpy);
    x.win = XCreateSimpleWindow(x.dpy, RootWindow(x.dpy, scr), 0, 0, w, h, 0, 0, BlackPixel(x.dpy, scr));
    XStoreName(x.dpy, x.win, title);
    XSelectInput(x.dpy, x.win, KeyPressMask | StructureNotifyMask);
    x.wm_delete = XInternAtom(x.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x.dpy, x.win, &x.wm_delete, 1);
    return true;
}

static void toggle_fullscreen(const XWin &x) {
    XEvent fs = {};
    fs.xclient.type = ClientMessage;
    fs.xclient.window = x.win;
    fs.xclient.message_type = XInternAtom(x.dpy, "_NET_WM_STATE", False);
    fs.xclient.format = 32;
    fs.xclient.data.l[0] = 2;  // _NET_WM_STATE_TOGGLE
    fs.xclient.data.l[1] = XInternAtom(x.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XSendEvent(x.dpy, DefaultRootWindow(x.dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &fs);
}

static void cmd_grid(LONG uid, std::vector<int> channels) {
    if (channels.empty()) channels = online_channels(uid);
    if (channels.empty()) { fprintf(stderr, "no online cameras\n"); return; }

    int w = 1920, h = 1080;
    XWin x;
    if (!open_window("Hikvision - all cameras", w, h, x)) return;
    Display *dpy = x.dpy;
    Window top = x.win;
    int scr = DefaultScreen(dpy);

    std::vector<Cell> cells;
    for (int ch : channels) {
        Window cw = XCreateSimpleWindow(dpy, top, 0, 0, 1, 1, 0, 0, BlackPixel(dpy, scr));
        XSelectInput(dpy, cw, ButtonPressMask);
        cells.push_back({ch, cw});
    }
    int zoomed = -1;
    layout(dpy, cells, w, h, zoomed);
    XMapWindow(dpy, top);
    XSync(dpy, False);
    for (Cell &c : cells) play(uid, c, false);
    fprintf(stderr, "showing %zu cameras: click = zoom in/out, f = fullscreen, m = motion overlays, q = quit\n",
            cells.size());

    while (!g_stop) {
        if (!XPending(dpy)) { usleep(20 * 1000); continue; }
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == x.wm_delete) break;
        if (ev.type == ConfigureNotify && ev.xconfigure.window == top &&
            (ev.xconfigure.width != w || ev.xconfigure.height != h)) {
            w = ev.xconfigure.width, h = ev.xconfigure.height;
            layout(dpy, cells, w, h, zoomed);
        } else if (ev.type == KeyPress) {
            KeySym k = XLookupKeysym(&ev.xkey, 0);
            if (k == XK_q || k == XK_Escape) break;
            if (k == XK_m) {
                g_overlays = !g_overlays;
                for (Cell &c : cells)
                    if (c.handle >= 0) set_overlays(c.handle, g_overlays);
                fprintf(stderr, "motion overlays %s\n", g_overlays ? "on" : "off");
            }
            if (k == XK_f) toggle_fullscreen(x);
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
            for (int i = 0; i < (int)cells.size(); i++) {
                if (cells[i].win != ev.xbutton.window) continue;
                int prev = zoomed;
                zoomed = zoomed >= 0 ? -1 : i;
                layout(dpy, cells, w, h, zoomed);
                play(uid, cells[i], zoomed >= 0);
                if (prev >= 0 && prev != i) play(uid, cells[prev], false);
                break;
            }
        }
    }
    for (Cell &c : cells)
        if (c.handle >= 0) NET_DVR_StopRealPlay(c.handle);
    XCloseDisplay(dpy);
}

struct Player {
    LONG uid;
    int channel;
    Cell live;           // live view drawn into the same window
    LONG pb = -1;        // playback handle
    time_t pos = 0;      // current (or remembered) playback time
    time_t begin = 0, end = 0;  // time window of the current playback session
    long long moved_ms = 0;
    long long seek_until_ms = 0;  // OSD time lags a seek; until then trust pos over the OSD
    int speed = 0;       // log2 of playback rate
    bool paused = false, stepping = false;
};

static void pb_stop(Player &p) {
    if (p.pb >= 0) NET_DVR_StopPlayBack(p.pb);
    if (p.live.handle >= 0) NET_DVR_StopRealPlay(p.live.handle);
    p.pb = p.live.handle = -1;
}

static bool pb_start(Player &p, time_t t) {
    pb_stop(p);
    t = std::min(t, time(nullptr) - 5);
    NET_DVR_VOD_PARA v = {};
    v.dwSize = sizeof(v);
    v.struIDInfo.dwSize = sizeof(v.struIDInfo);
    v.struIDInfo.dwChannel = p.channel;
    v.struBeginTime = to_dvr(t);
    v.struEndTime = to_dvr(time(nullptr));
    p.begin = t;
    p.end = time(nullptr);
    v.hWnd = (HWND)p.live.win;
    p.pb = NET_DVR_PlayBackByTime_V40(p.uid, &v);
    if (p.pb < 0 || !NET_DVR_PlayBackControl_V40(p.pb, NET_DVR_PLAYSTART)) {
        LONG err = NET_DVR_GetLastError();
        fprintf(stderr, "playback from %s failed: error %d (%s)\n", fmt(t).c_str(), err, NET_DVR_GetErrorMsg(&err));
        pb_stop(p);
        return false;
    }
    set_overlays(p.pb, g_overlays);
    p.pos = t;
    p.moved_ms = now_ms();
    p.speed = 0;
    p.paused = p.stepping = false;
    return true;
}

static void pb_seek(Player &p, time_t t) {
    t = std::min(t, time(nullptr) - 5);
    // The NVR accepts but ignores seeks outside the session window (playback just freezes).
    if (p.live.handle >= 0 || t < p.begin || t > p.end) { pb_start(p, t); return; }
    NET_DVR_TIME d = to_dvr(t);
    if (!NET_DVR_PlayBackControl_V40(p.pb, NET_DVR_PLAYSETTIME, &d, sizeof(d))) {
        pb_start(p, t);
        return;
    }
    p.pos = t;
    p.moved_ms = now_ms();
    p.seek_until_ms = now_ms() + 3000;
}

static void pb_title(Display *dpy, Window win, Player &p) {
    char title[200];
    if (p.live.handle >= 0) {
        snprintf(title, sizeof(title), "ch %d  LIVE   (l = back to playback at %s)", p.channel, fmt(p.pos).c_str());
    } else if (p.pb >= 0) {
        NET_DVR_TIME d = {};
        if (NET_DVR_GetPlayBackOsdTime(p.pb, &d) && d.dwYear > 2000) {
            time_t t = from_dvr(d);
            bool stale = now_ms() < p.seek_until_ms && labs(t - p.pos) > 3;
            if (!stale && t != p.pos) p.pos = t, p.moved_ms = now_ms();
        }
        char speed[16] = "";
        if (p.speed > 0) snprintf(speed, sizeof(speed), "  x%d", 1 << p.speed);
        if (p.speed < 0) snprintf(speed, sizeof(speed), "  x1/%d", 1 << -p.speed);
        bool stalled = !p.paused && !p.stepping && now_ms() - p.moved_ms > 5000;
        snprintf(title, sizeof(title), "ch %d  PLAYBACK  %s%s%s%s", p.channel, fmt(p.pos).c_str(), speed,
                 p.paused || p.stepping ? "  [paused]" : "", stalled ? "  - no recording here?" : "");
    } else {
        snprintf(title, sizeof(title), "ch %d  stopped (playback failed, see terminal)", p.channel);
    }
    XStoreName(dpy, win, title);
    XFlush(dpy);
}

static void cmd_play(LONG uid, int channel, time_t start) {
    XWin x;
    if (!open_window("Hikvision playback", 1920, 1080, x)) return;
    XMapWindow(x.dpy, x.win);
    XSync(x.dpy, False);

    Player p{uid, channel, {channel, x.win}};
    if (!pb_start(p, start)) { XCloseDisplay(x.dpy); return; }
    fprintf(stderr,
            "keys: space pause | <- -> 10 s | shift+<- -> or down/up 1 min | PgDn/PgUp 10 min\n"
            "      + - speed | 0 normal speed | . next frame | l live/playback | f fullscreen | m overlays | q quit\n");

    long long next_title = 0;
    while (!g_stop) {
        if (now_ms() >= next_title) {
            pb_title(x.dpy, x.win, p);
            next_title = now_ms() + 500;
        }
        if (!XPending(x.dpy)) { usleep(20 * 1000); continue; }
        XEvent ev;
        XNextEvent(x.dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == x.wm_delete) break;
        if (ev.type != KeyPress) continue;

        char buf[8];
        KeySym k;
        XLookupString(&ev.xkey, buf, sizeof(buf), &k, nullptr);
        bool shift = ev.xkey.state & ShiftMask;
        bool live = p.live.handle >= 0;
        // In live mode a backward jump starts playback relative to now ("instant replay").
        time_t base = live ? time(nullptr) : p.pos;
        long delta = 0;
        if (k == XK_Left) delta = shift ? -60 : -10;
        if (k == XK_Right) delta = shift ? 60 : 10;
        if (k == XK_Down) delta = -60;
        if (k == XK_Up) delta = 60;
        if (k == XK_Next) delta = -600;
        if (k == XK_Prior) delta = 600;

        if (k == XK_q || k == XK_Escape) break;
        if (k == XK_f) toggle_fullscreen(x);
        if (k == XK_m) {
            g_overlays = !g_overlays;
            set_overlays(live ? p.live.handle : p.pb, g_overlays);
        }
        if (k == XK_l) {
            if (live) {
                pb_start(p, p.pos);
            } else {
                pb_stop(p);
                play(uid, p.live, true);
            }
        }
        if (delta && (!live || delta < 0)) pb_seek(p, base + delta);
        if (live || p.pb < 0) { next_title = 0; continue; }

        if (k == XK_space) {
            // Leaving frame-by-frame mode needs NORMAL; plain pause is undone by RESTART.
            DWORD code = p.stepping ? NET_DVR_PLAYNORMAL : p.paused ? NET_DVR_PLAYRESTART : NET_DVR_PLAYPAUSE;
            if (NET_DVR_PlayBackControl_V40(p.pb, code)) {
                if (p.stepping) p.speed = 0;
                p.paused = !(p.paused || p.stepping);
                p.stepping = false;
                p.moved_ms = now_ms();
            }
        }
        if ((k == XK_plus || k == XK_equal || k == XK_KP_Add) && p.speed < 4 &&
            NET_DVR_PlayBackControl_V40(p.pb, NET_DVR_PLAYFAST))
            p.speed++;
        if ((k == XK_minus || k == XK_KP_Subtract) && p.speed > -4 &&
            NET_DVR_PlayBackControl_V40(p.pb, NET_DVR_PLAYSLOW))
            p.speed--;
        if (k == XK_0 && NET_DVR_PlayBackControl_V40(p.pb, NET_DVR_PLAYNORMAL)) {
            p.speed = 0;
            p.paused = p.stepping = false;
        }
        if (k == XK_period && NET_DVR_PlayBackControl_V40(p.pb, NET_DVR_PLAYFRAME)) p.stepping = true;
        next_title = 0;
    }
    pb_stop(p);
    XCloseDisplay(x.dpy);
}

static void cmd_recordings(LONG uid, int channel, time_t day) {
    struct tm lt;
    localtime_r(&day, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    time_t from = mktime(&lt);

    NET_DVR_FILECOND_V40 c = {};
    c.lChannel = channel;
    c.dwFileType = 0xff;
    c.dwIsLocked = 0xff;
    c.struStartTime = to_dvr(from);
    c.struStopTime = to_dvr(from + 86399);
    LONG f = NET_DVR_FindFile_V40(uid, &c);
    if (f < 0) die_sdk("NET_DVR_FindFile_V40");

    struct Seg { time_t from, to; int type; double mb; };
    std::vector<Seg> segs;
    while (!g_stop) {
        NET_DVR_FINDDATA_V40 d = {};
        LONG r = NET_DVR_FindNextFile_V40(f, &d);
        if (r == NET_DVR_ISFINDING) { usleep(100 * 1000); continue; }
        if (r != NET_DVR_FILE_SUCCESS) {
            if (r != NET_DVR_FILE_NOFIND && r != NET_DVR_NOMOREFILE) fprintf(stderr, "search ended with code %d\n", r);
            break;
        }
        // Files that span midnight are returned whole; show only the part within this day.
        Seg s{std::max(from_dvr(d.struStartTime), from), std::min(from_dvr(d.struStopTime), from + 86399),
              d.byFileType, d.dwFileSize / 1048576.0};
        if (!segs.empty() && segs.back().type == s.type && s.from - segs.back().to <= 2)
            segs.back().to = s.to, segs.back().mb += s.mb;
        else
            segs.push_back(s);
    }
    NET_DVR_FindClose_V30(f);

    printf("channel %d, %s:\n", channel, fmt(from, "%Y-%m-%d").c_str());
    if (segs.empty()) printf("  no recordings\n");
    long total = 0;
    for (const Seg &s : segs) {
        static const char *types[] = {"schedule", "motion", "alarm", "motion|alarm", "motion&alarm", "command", "manual"};
        long len = s.to - s.from;
        total += len;
        printf("  %s - %s  %3ldh%02ldm  %-12s %8.0f MB\n", fmt(s.from, "%H:%M:%S").c_str(), fmt(s.to, "%H:%M:%S").c_str(),
               len / 3600, len / 60 % 60, s.type < 7 ? types[s.type] : std::to_string(s.type).c_str(), s.mb);
    }
    if (!segs.empty()) printf("  total %ldh%02ldm\n", total / 3600, total / 60 % 60);
}

static int cmd_export(LONG uid, int channel, time_t from, time_t to, const char *out) {
    if (to <= from) { fprintf(stderr, "end time must be after start time\n"); return 2; }
    NET_DVR_PLAYCOND pc = {};
    pc.dwChannel = channel;
    pc.struStartTime = to_dvr(from);
    pc.struStopTime = to_dvr(to);
    LONG h = NET_DVR_GetFileByTime_V40(uid, (char *)out, &pc);
    if (h < 0) die_sdk("NET_DVR_GetFileByTime_V40");
    if (!NET_DVR_PlayBackControl(h, NET_DVR_PLAYSTART, 0, nullptr)) die_sdk("NET_DVR_PlayBackControl(PLAYSTART)");

    fprintf(stderr, "downloading channel %d %s .. %s\n", channel, fmt(from).c_str(), fmt(to, "%H:%M:%S").c_str());
    int rc = 1, last = -1;
    while (!g_stop) {
        int pos = NET_DVR_GetDownloadPos(h);
        if (pos < 0 || pos > 100) { fprintf(stderr, "\ndownload failed (position %d)\n", pos); break; }
        if (pos != last) fprintf(stderr, "\r  %3d%%", last = pos);
        if (pos == 100) { rc = 0; fprintf(stderr, "\n"); break; }
        usleep(500 * 1000);
    }
    NET_DVR_StopGetFile(h);
    return rc;
}

static void CALLBACK on_data(LONG, DWORD type, BYTE *buf, DWORD len, void *) {
    // The 40-byte Hikvision "IMKH" system header is not part of the PS stream.
    if (type == NET_DVR_SYSHEAD || g_stop) return;
    if (fwrite(buf, 1, len, stdout) != len || fflush(stdout) != 0) g_stop = 1;
}

static void cmd_stream(LONG uid, int channel, bool sub) {
    if (isatty(STDOUT_FILENO)) {
        fprintf(stderr, "refusing to write video to a terminal; pipe into ffplay/ffmpeg\n");
        exit(1);
    }
    NET_DVR_PREVIEWINFO pv = {};
    pv.lChannel = channel;
    pv.dwStreamType = sub ? 1 : 0;
    pv.dwLinkMode = 0;  // TCP
    pv.bBlocked = 1;
    LONG h = NET_DVR_RealPlay_V40(uid, &pv, on_data, nullptr);
    if (h < 0) die_sdk("NET_DVR_RealPlay_V40");
    fprintf(stderr, "streaming channel %d (%s stream), Ctrl+C to stop\n", channel, sub ? "sub" : "main");
    while (!g_stop) usleep(200 * 1000);
    NET_DVR_StopRealPlay(h);
}

int main(int argc, char **argv) {
    const char *cmd = argc >= 3 ? argv[2] : "";
    int need = !strcmp(cmd, "stream") || !strcmp(cmd, "recordings") ? 4 : !strcmp(cmd, "play") ? 5
             : !strcmp(cmd, "export") ? 7 : 3;
    if (argc < need) {
        fprintf(stderr,
                "usage: %1$s <config> list\n"
                "       %1$s <config> stream <channel> [sub]\n"
                "       %1$s <config> grid [channel...]\n"
                "       %1$s <config> play <channel> <time>\n"
                "       %1$s <config> recordings <channel> [date]\n"
                "       %1$s <config> export <channel> <from> <to> <file.ps>\n",
                argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // Parse times before connecting so typos fail fast.
    time_t t1 = 0, t2 = 0;
    if (!strcmp(cmd, "play") || !strcmp(cmd, "export")) t1 = parse_time(argv[4]);
    if (!strcmp(cmd, "export")) t2 = parse_time(argv[5]);
    if (!strcmp(cmd, "recordings")) t1 = argc > 4 ? parse_time(argv[4]) : time(nullptr);

    init_sdk();
    NET_DVR_DEVICEINFO_V40 info = {};
    LONG uid = login(argv[1], &info);
    int rc = 0;

    if (!strcmp(cmd, "list"))
        cmd_list(uid, info);
    else if (!strcmp(cmd, "stream"))
        cmd_stream(uid, atoi(argv[3]), argc > 4 && !strcmp(argv[4], "sub"));
    else if (!strcmp(cmd, "grid")) {
        std::vector<int> chans;
        for (int i = 3; i < argc; i++) chans.push_back(atoi(argv[i]));
        cmd_grid(uid, chans);
    } else if (!strcmp(cmd, "play"))
        cmd_play(uid, atoi(argv[3]), t1);
    else if (!strcmp(cmd, "recordings"))
        cmd_recordings(uid, atoi(argv[3]), t1);
    else if (!strcmp(cmd, "export"))
        rc = cmd_export(uid, atoi(argv[3]), t1, t2, argv[6]);
    else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        rc = 2;
    }

    NET_DVR_Logout(uid);
    NET_DVR_Cleanup();
    return rc;
}
