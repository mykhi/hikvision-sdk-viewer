#include "view.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "nvr.h"
#include "util.h"

struct Cell {
    int channel;
    Window win;
    LONG handle = -1;
};

static bool g_overlays = false;

static bool set_overlays(LONG handle, bool on) {
    bool ok = true;
    for (int type : {RENDER_ANA_INTEL_DATA, RENDER_MD, RENDER_ADD_POS, RENDER_ADD_PIC, RENDER_FIRE_DETCET, RENDER_TEM})
        ok &= sdk::NET_DVR_RenderPrivateData(handle, type, on) != 0;
    return ok;
}

static void play(LONG uid, Cell &c, bool main_stream) {
    if (c.handle >= 0) sdk::NET_DVR_StopRealPlay(c.handle);
    NET_DVR_PREVIEWINFO pv = {};
    pv.lChannel = c.channel;
    pv.dwStreamType = main_stream ? 0 : 1;
    pv.hPlayWnd = (HWND)c.win;
    pv.bBlocked = 0;
    pv.dwDisplayBufNum = 1;  // render immediately instead of smoothing with a frame buffer
    c.handle = sdk::NET_DVR_RealPlay_V40(uid, &pv, nullptr, nullptr);
    if (c.handle < 0)
        fprintf(stderr, "channel %d: live view failed: %s\n", c.channel, sdk::last_error().c_str());
    else if (!set_overlays(c.handle, g_overlays))
        fprintf(stderr, "channel %d: overlay setting failed: %s\n", c.channel, sdk::last_error().c_str());
}

static void layout(Display *dpy, std::vector<Cell> &cells, int w, int h, int zoomed) {
    int n = cells.size(), cols = 1;
    while (cols * cols < n) cols++;
    int rows = (n + cols - 1) / cols;
    for (int i = 0; i < n; i++) {
        if (zoomed >= 0 && i != zoomed) { XUnmapWindow(dpy, cells[i].win); continue; }
        if (zoomed >= 0 || n == 1)
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
    XClassHint cls = {(char *)"hikview", (char *)"hikview"};  // matches StartupWMClass in the menu entry
    XSetClassHint(x.dpy, x.win, &cls);
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

void cmd_grid(LONG uid, std::vector<int> channels) {
    if (channels.empty()) channels = online_channels(uid);
    if (channels.empty()) { fprintf(stderr, "no online cameras\n"); return; }
    bool single = channels.size() == 1;

    int w = 1920, h = 1080;
    std::string title = single ? "Hikvision - channel " + std::to_string(channels[0]) : "Hikvision - all cameras";
    XWin x;
    if (!open_window(title.c_str(), w, h, x)) return;
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
    for (Cell &c : cells) play(uid, c, single);
    fprintf(stderr, "%s: f = fullscreen, m = motion overlays, q = quit\n",
            single ? "live view" : "live grid: click = zoom in/out");

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
            }
            if (k == XK_f) toggle_fullscreen(x);
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button1 && !single) {
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
        if (c.handle >= 0) sdk::NET_DVR_StopRealPlay(c.handle);
    XCloseDisplay(dpy);
}

struct Player {
    LONG uid;
    int channel;
    Cell live;                    // live view drawn into the same window
    LONG pb = -1;                 // playback handle
    time_t pos = 0;               // current (or remembered) playback time
    time_t begin = 0, end = 0;    // time window of the current playback session
    long long moved_ms = 0;
    long long seek_until_ms = 0;  // OSD time lags a seek; until then trust pos over the OSD
    int speed = 0;                // log2 of playback rate
    bool paused = false, stepping = false;
};

static bool pb_control(LONG handle, DWORD code, void *in = nullptr, DWORD in_len = 0) {
    return sdk::NET_DVR_PlayBackControl_V40(handle, code, in, in_len, nullptr, nullptr);
}

static void pb_stop(Player &p) {
    if (p.pb >= 0) sdk::NET_DVR_StopPlayBack(p.pb);
    if (p.live.handle >= 0) sdk::NET_DVR_StopRealPlay(p.live.handle);
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
    p.pb = sdk::NET_DVR_PlayBackByTime_V40(p.uid, &v);
    if (p.pb < 0 || !pb_control(p.pb, NET_DVR_PLAYSTART)) {
        fprintf(stderr, "playback from %s failed: %s\n", fmt(t).c_str(), sdk::last_error().c_str());
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
    if (!pb_control(p.pb, NET_DVR_PLAYSETTIME, &d, sizeof(d))) {
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
        if (sdk::NET_DVR_GetPlayBackOsdTime(p.pb, &d) && d.dwYear > 2000) {
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

void cmd_play(LONG uid, int channel, time_t start) {
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
            if (pb_control(p.pb, code)) {
                if (p.stepping) p.speed = 0;
                p.paused = !(p.paused || p.stepping);
                p.stepping = false;
                p.moved_ms = now_ms();
            }
        }
        if ((k == XK_plus || k == XK_equal || k == XK_KP_Add) && p.speed < 4 && pb_control(p.pb, NET_DVR_PLAYFAST))
            p.speed++;
        if ((k == XK_minus || k == XK_KP_Subtract) && p.speed > -4 && pb_control(p.pb, NET_DVR_PLAYSLOW))
            p.speed--;
        if (k == XK_0 && pb_control(p.pb, NET_DVR_PLAYNORMAL)) {
            p.speed = 0;
            p.paused = p.stepping = false;
        }
        if (k == XK_period && pb_control(p.pb, NET_DVR_PLAYFRAME)) p.stepping = true;
        next_title = 0;
    }
    pb_stop(p);
    XCloseDisplay(x.dpy);
}
