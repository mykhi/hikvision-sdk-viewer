# hikvision-sdk-viewer

`hikview` is a live view and playback app for Hikvision NVRs on Linux, built on Hikvision's own Device Network SDK (HCNetSDK).

It talks to the NVR's **SDK port** (8000 by default). That's the same port iVMS-4200 uses, so it also works when RTSP and HTTP are not reachable, for example when only the SDK port is forwarded. Video is decoded and drawn by the SDK into plain X11 windows. There's no Qt, no browser and no transcoding.

- **Live grid** of all online cameras. Click a camera to zoom in at full quality. Latency is about 1–2 s over the internet.
- **Playback** of NVR recordings from any time, with pause, seeking, speed, frame step and a live/playback toggle.
- **Recordings list**: which time ranges of a day are recorded.
- **Export** to `.mp4` without re-encoding, and **snapshots**.
- **Several NVRs**, each with its own login, stored in the system keyring.

Tested with a DS-7616NI-K2 NVR (16 IP channels, H.264 substreams and H.265 main streams) on Ubuntu 24.04 / X11.

> Not affiliated with or endorsed by Hikvision. The Hikvision SDK is **not** included and can't be. You download it from Hikvision once, and its license applies to it.

## Install

1. Download `hikview-…-linux-x86_64.tar.gz` from [Releases](https://github.com/mykhi/hikvision-sdk-viewer/releases) and unpack it.
2. Download **Device Network SDK_Linux64** from Hikvision's [SDK page](https://www.hikvision.com/en/support/download/sdk/), accepting their license. You get a file like `EN-HCNetSDKV6.1.11.40_build20260922_linux64.zip`. Don't unpack it.
3. Run setup:
   ```bash
   sudo apt install unzip libsecret-tools   # libsecret-tools is optional, for the keyring
   ./hikview setup --sdk ~/Downloads/EN-HCNetSDKV*_linux64.zip
   ```
   Setup does five things:
   - installs `hikview` to `~/.local/bin`
   - installs the SDK libraries to `~/.local/share/hikview/sdk`
   - asks for the NVR address (`host:port`, the SDK port), user and password
   - adds **Hikvision cameras** to the app menu
   - connects once to check everything works

That's it. Start it from the app menu or run `hikview`.

`hikview uninstall` removes the program, SDK and menu entries; add `--purge` to also delete saved logins.

## Usage

```
hikview [-d NAME | -c FILE] [command]

  (none), grid [CH...]      live view of all online cameras (or the given channels)
  live CH                   one camera, full quality
  play CH TIME              play back recordings
  recordings CH [DATE]      recorded time ranges of a day (default today)
  export CH FROM TO FILE    save a recording as .mp4 (needs ffmpeg) or raw .ps
  snap CH FILE.jpg          snapshot taken by the NVR
  list                      device info and channels
  stream CH [sub]           raw MPEG-PS to stdout, e.g. | ffplay -f mpeg -i -
  setup [--sdk ZIP]         install / add or change a device login
  uninstall [--purge]       remove hikview
```

`TIME` and `DATE` accept `"2026-10-13 21:00"`, `21:00` (today), `2026-10-13`, or relative `-90s`, `-10m`, `-2h`, `-1d`.

Channel numbers come from `hikview list`; IP cameras on NVRs usually start at 33.

```bash
hikview                                   # live grid
hikview live 35
hikview play 33 -10m
hikview recordings 33 -1d
hikview export 33 "2026-10-13 21:00" "2026-10-13 21:05" clip.mp4
```

### Live grid keys

| Key / mouse | Action |
|---|---|
| click a camera | zoom to it (main stream); click again to go back |
| `f` | fullscreen |
| `m` | motion / smart-event boxes drawn by the SDK on or off (off by default) |
| `q`, Esc | quit |

### Playback keys

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

### Several NVRs

```bash
hikview -d office setup      # adds a device "office" and an app menu entry for it
hikview -d office            # live grid of the office NVR
hikview -d office play 33 -1h
```

Device configs live in `~/.config/hikview/devices/`. Each is a small text file: line 1 `host:port`, line 2 user, and an optional line 3 password. The program refuses a password in the file unless the file is `chmod 600`. `-c FILE` uses such a file directly.

The password is taken from the first of these that's available:
1. the config file
2. `$HIK_PASSWORD`
3. the system keyring (GNOME Keyring / KWallet via `secret-tool`)
4. a hidden prompt in the terminal

## Build from source

```bash
sudo apt install build-essential libx11-dev
unzip EN-HCNetSDKV*_linux64.zip '*/incEn/*' -d sdk   # only HCNetSDK.h is needed to build
make            # -> build/hikview
make dist       # -> dist/hikview-<version>-linux-x86_64.tar.gz
```

The binary doesn't link against the SDK. It loads `libhcnetsdk.so` from `~/.local/share/hikview/sdk` at startup, which is why it can be shipped on its own. Releases are built by GitHub Actions on Ubuntu 22.04 when a `v*` tag is pushed.

## Notes

- The SDK prints library-loading messages on startup (`load libavcodec ... failed`, `Cannot load libcuda.so.1`). They're harmless: it falls back to its own decoders and VA-API.
- Times are local time; the NVR clock is assumed to be in the same time zone (`hikview list` shows both clocks).
- When started from the app menu there's no terminal, so the password must be saved (keyring or config file) and errors aren't shown. Run `hikview` in a terminal to see them.
- Hikvision devices lock the account for a while after several wrong passwords.
- Exposing the SDK port to the internet is risky: older Hikvision firmware has known remote vulnerabilities. Prefer a VPN (e.g. WireGuard on the router) and keep the firmware updated.
- X11 only; Wayland sessions via XWayland are untested.

## License

MIT for the code in this repository. The Hikvision SDK is subject to Hikvision's own license and is not distributed here.
