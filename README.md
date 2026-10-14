# hikvision-sdk-viewer

Live view and playback for Hikvision NVRs on Linux, using Hikvision's own Device Network SDK (HCNetSDK).

It talks to the NVR's **SDK port** (8000 by default). That's the same port iVMS-4200 uses, so it also works when RTSP and HTTP are not reachable, for example when only the SDK port is forwarded. Video is decoded and drawn by the SDK into plain X11 windows. There's no Qt, no browser and no transcoding: one C++ file and a small shell wrapper.

- **Live grid**: all online cameras in one window. Click a camera to zoom it to full-quality video. Latency is about 1–2 s over the internet.
- **Playback** of NVR recordings from any time, with pause, seeking, speed, frame step and a live/playback toggle.
- **Recordings list**: which time ranges of a day are recorded.
- **Export** a time range to `.mp4` without re-encoding.

Tested with a DS-7616NI-K2 NVR (16 IP channels, H.264 substreams and H.265 main streams) on Ubuntu 24.04 / X11.

> Not affiliated with or endorsed by Hikvision. The Hikvision SDK is **not** included; you download it from Hikvision yourself and its license applies to it.

## Requirements

- Linux x86_64 with an X11 session (Wayland via XWayland is untested)
- `g++`, `make`, `unzip`, `libx11-dev`
- `ffmpeg` (`ffmpeg`, `ffplay`, `ffprobe`): only for `export`, `snap` and single-camera `ffplay` view

```bash
sudo apt install build-essential unzip libx11-dev ffmpeg
```

## Setup

1. Download **Device Network SDK_Linux64** from Hikvision's [HiTools / SDK page](https://www.hikvision.com/en/support/download/sdk/), accepting their license agreement. You get a file like `EN-HCNetSDKV6.1.11.40_build20260922_linux64.zip`.
2. Unpack and build:
   ```bash
   ./setup-sdk.sh ~/Downloads/EN-HCNetSDKV*_linux64.zip
   ```
3. Enter the NVR address and login:
   ```bash
   sudo apt install libsecret-tools   # optional, lets setup store the password in the system keyring
   ./hikview.sh setup
   ```
   This writes `~/.config/hikview/config` (address and user only) and stores the password in the keyring (GNOME Keyring / KWallet via Secret Service). Without `secret-tool`, the password goes into the config file with mode 600.

   The password is taken from the first of these that is available:
   1. line 3 of the config file (refused unless the file is `chmod 600`)
   2. the `HIK_PASSWORD` environment variable
   3. the system keyring (`secret-tool lookup service hikview address <host:port> user <user>`)
   4. a hidden prompt in the terminal

   For several devices, keep one config per device and select it with `HIK_CONFIG=/path/to/file`. See `config.example` for the format.

   > Hikvision devices lock the account for a while after several wrong passwords.
4. Check it:
   ```bash
   ./hikview.sh list
   ```
   ```
   serial:          DS-7616NI-K2...
   IP channels:     16 (first = 33)
   NVR clock:       2026-10-14 08:41:46  (this PC: 08:41:48)
   NVR NTP:         on (server ..., every 60 h)
     channel  33  IP camera 10.0.0.75        time sync from NVR: default  online
     ...
   ```

## Usage

### Live grid

```bash
./hikview.sh grid            # all online cameras
./hikview.sh grid 33 35 41   # selected channels
```

| Key / mouse | Action |
|---|---|
| click a camera | zoom to it (switches to the main stream); click again to go back |
| `f` | fullscreen |
| `m` | motion / smart-event boxes drawn by the SDK on or off (off by default) |
| `q`, Esc | quit |

### Playback

```bash
./hikview.sh play 33 -10m                 # 10 minutes ago
./hikview.sh play 33 21:00                # today 21:00
./hikview.sh play 33 "2026-10-13 21:00"
```

The window title shows the playback time, speed and state.

| Key | Action |
|---|---|
| Space | pause / resume |
| ← / → | −/+ 10 s |
| Shift+← / → or ↓ / ↑ | −/+ 1 min |
| PgDn / PgUp | −/+ 10 min |
| `+` / `-` / `0` | faster / slower / normal speed (1/16×–16×) |
| `.` | next frame (Space resumes) |
| `l` | switch to live and back (resumes where you were); ← or ↓ in live = instant replay |
| `f` / `m` / `q` | fullscreen / overlays / quit |

### Recordings and export

```bash
./hikview.sh recordings 33               # today
./hikview.sh recordings 33 2026-10-13    # or -1d
./hikview.sh export 33 "2026-10-13 21:00" "2026-10-13 21:05" clip.mp4
```

### Single camera through ffplay

```bash
./hikview.sh 33          # main stream
./hikview.sh 33 sub      # substream
./hikview.sh snap 33 frame.jpg
```

`hikview <config> stream <channel>` writes the raw MPEG-PS stream to stdout, so you can pipe it into anything that reads `ffmpeg -f mpeg`.

## Notes

- The SDK prints library-loading messages on startup (`load libavcodec ... failed`, `Cannot load libcuda.so.1`). They're harmless: it falls back to its own decoders and VA-API.
- Times are local time; the NVR clock is assumed to be in the same time zone as the PC (`list` shows both).
- Exposing the SDK port to the internet is risky: older Hikvision firmware has known remote vulnerabilities. Prefer a VPN (e.g. WireGuard on the router) and keep the firmware updated.

## License

MIT for the code in this repository. The Hikvision SDK is subject to Hikvision's own license and is not distributed here.
