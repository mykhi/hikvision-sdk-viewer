#include "nvr.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "util.h"

std::vector<int> online_channels(LONG uid) {
    std::vector<int> out;
    NET_DVR_IPPARACFG_V40 ip = {};
    DWORD ret = 0;
    if (!sdk::NET_DVR_GetDVRConfig(uid, NET_DVR_GET_IPPARACFG_V40, 0, &ip, sizeof(ip), &ret)) return out;
    for (DWORD i = 0; i < ip.dwDChanNum && i < MAX_CHANNUM_V30; i++) {
        const NET_DVR_STREAM_MODE &m = ip.struStreamMode[i];
        if (m.byGetStreamType == 0 && m.uGetStream.struChanInfo.byEnable) out.push_back(ip.dwStartDChan + i);
    }
    return out;
}

void cmd_list(LONG uid, const NET_DVR_DEVICEINFO_V40 &info) {
    const NET_DVR_DEVICEINFO_V30 &d = info.struDeviceV30;
    char serial[SERIALNO_LEN + 1] = {0};
    memcpy(serial, d.sSerialNumber, SERIALNO_LEN);
    int ip_chans = d.byIPChanNum + d.byHighDChanNum * 256;

    printf("serial:          %s\n", serial);
    printf("analog channels: %d (first = %d)\n", d.byChanNum, d.byStartChan);
    printf("IP channels:     %d (first = %d)\n", ip_chans, d.byStartDChan);

    NET_DVR_TIME t = {};
    DWORD n = 0;
    if (sdk::NET_DVR_GetDVRConfig(uid, NET_DVR_GET_TIMECFG, 0, &t, sizeof(t), &n))
        printf("NVR clock:       %s  (this PC: %s)\n", fmt(from_dvr(t)).c_str(), fmt(time(nullptr), "%H:%M:%S").c_str());
    NET_DVR_NTPPARA ntp = {};
    if (sdk::NET_DVR_GetDVRConfig(uid, NET_DVR_GET_NTPCFG, 0, &ntp, sizeof(ntp), &n))
        printf("NVR NTP:         %s (server %.64s, every %u h)\n", ntp.byEnableNTP ? "on" : "off",
               (const char *)ntp.sNTPServer, ntp.wInterval);

    for (int i = 0; i < d.byChanNum; i++)
        printf("  channel %3d  analog\n", d.byStartChan + i);

    if (ip_chans == 0) return;
    NET_DVR_IPPARACFG_V40 ip = {};
    DWORD ret = 0;
    if (!sdk::NET_DVR_GetDVRConfig(uid, NET_DVR_GET_IPPARACFG_V40, 0, &ip, sizeof(ip), &ret)) {
        fprintf(stderr, "IP channel config unavailable: %s\n", sdk::last_error().c_str());
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

void cmd_recordings(LONG uid, int channel, time_t day) {
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
    LONG f = sdk::NET_DVR_FindFile_V40(uid, &c);
    if (f < 0) sdk::die("NET_DVR_FindFile_V40");

    struct Seg { time_t from, to; int type; double mb; };
    std::vector<Seg> segs;
    while (!g_stop) {
        NET_DVR_FINDDATA_V40 d = {};
        LONG r = sdk::NET_DVR_FindNextFile_V40(f, &d);
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
    sdk::NET_DVR_FindClose_V30(f);

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

static bool download(LONG uid, int channel, time_t from, time_t to, const char *path) {
    NET_DVR_PLAYCOND pc = {};
    pc.dwChannel = channel;
    pc.struStartTime = to_dvr(from);
    pc.struStopTime = to_dvr(to);
    LONG h = sdk::NET_DVR_GetFileByTime_V40(uid, (char *)path, &pc);
    if (h < 0) sdk::die("NET_DVR_GetFileByTime_V40");
    if (!sdk::NET_DVR_PlayBackControl(h, NET_DVR_PLAYSTART, 0, nullptr)) sdk::die("NET_DVR_PlayBackControl(PLAYSTART)");

    fprintf(stderr, "downloading channel %d %s .. %s\n", channel, fmt(from).c_str(), fmt(to, "%H:%M:%S").c_str());
    bool ok = false;
    int last = -1;
    while (!g_stop) {
        int pos = sdk::NET_DVR_GetDownloadPos(h);
        if (pos < 0 || pos > 100) { fprintf(stderr, "\ndownload failed (position %d)\n", pos); break; }
        if (pos != last) fprintf(stderr, "\r  %3d%%", last = pos);
        if (pos == 100) { ok = true; fprintf(stderr, "\n"); break; }
        usleep(500 * 1000);
    }
    sdk::NET_DVR_StopGetFile(h);
    return ok;
}

int cmd_export(LONG uid, int channel, time_t from, time_t to, const char *out) {
    if (to <= from) { fprintf(stderr, "end time must be after start time\n"); return 2; }
    size_t len = strlen(out);
    if (len > 3 && !strcmp(out + len - 3, ".ps")) return download(uid, channel, from, to, out) ? 0 : 1;

    if (!have_command("ffmpeg") || !have_command("ffprobe")) {
        fprintf(stderr, "converting to %s needs ffmpeg (apt install ffmpeg); or export to a .ps file instead\n", out);
        return 1;
    }
    char tmp[] = "/tmp/hikview-XXXXXX.ps";
    int fd = mkstemps(tmp, 3);
    if (fd < 0) { perror("mkstemps"); return 1; }
    close(fd);
    int rc = 1;
    if (download(uid, channel, from, to, tmp)) {
        std::string codec = run_line({"ffprobe", "-v", "error", "-f", "mpeg", "-select_streams", "v:0", "-show_entries",
                                      "stream=codec_name", "-of", "csv=p=0", tmp});
        std::vector<const char *> args = {"ffmpeg", "-loglevel", "error", "-y", "-f", "mpeg", "-i", tmp, "-map", "0:v",
                                          "-c", "copy", "-movflags", "+faststart"};
        if (codec == "hevc") args.insert(args.end(), {"-tag:v", "hvc1"});  // lets QuickTime/browsers play HEVC in mp4
        args.push_back(out);
        rc = run(args) == 0 ? 0 : 1;
        if (rc == 0) fprintf(stderr, "saved %s\n", out);
    }
    unlink(tmp);
    return rc;
}

int cmd_snap(LONG uid, int channel, const char *out) {
    NET_DVR_JPEGPARA jp = {};
    jp.wPicSize = 0xff;  // resolution of the current stream
    jp.wPicQuality = 0;
    if (!sdk::NET_DVR_CaptureJPEGPicture(uid, channel, &jp, (char *)out)) {
        fprintf(stderr, "snapshot failed: %s\n", sdk::last_error().c_str());
        return 1;
    }
    fprintf(stderr, "saved %s\n", out);
    return 0;
}

static void CALLBACK on_data(LONG, DWORD type, BYTE *buf, DWORD len, void *) {
    // The 40-byte Hikvision "IMKH" system header is not part of the PS stream.
    if (type == NET_DVR_SYSHEAD || g_stop) return;
    if (fwrite(buf, 1, len, stdout) != len || fflush(stdout) != 0) g_stop = 1;
}

void cmd_stream(LONG uid, int channel, bool sub) {
    if (isatty(STDOUT_FILENO)) {
        fprintf(stderr, "refusing to write video to a terminal; pipe it into ffplay/ffmpeg (-f mpeg -i -)\n");
        exit(1);
    }
    NET_DVR_PREVIEWINFO pv = {};
    pv.lChannel = channel;
    pv.dwStreamType = sub ? 1 : 0;
    pv.dwLinkMode = 0;  // TCP
    pv.bBlocked = 1;
    LONG h = sdk::NET_DVR_RealPlay_V40(uid, &pv, on_data, nullptr);
    if (h < 0) sdk::die("NET_DVR_RealPlay_V40");
    fprintf(stderr, "streaming channel %d (%s stream), Ctrl+C to stop\n", channel, sub ? "sub" : "main");
    while (!g_stop) usleep(200 * 1000);
    sdk::NET_DVR_StopRealPlay(h);
}
