#!/usr/bin/env bash
# Usage: hikview.sh setup
#        hikview.sh list | grid [channel...] | <channel> [sub] | snap <channel> <out.jpg>
#        hikview.sh play <channel> <time>
#        hikview.sh recordings <channel> [date]
#        hikview.sh export <channel> <from> <to> <out.mp4>
# <time>: "2026-10-13 21:00[:SS]", "21:00" (today), "2026-10-13", or relative "-90s" "-10m" "-2h" "-1d"
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
cfg=${HIK_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/hikview/config}

if [[ ${1:-} == setup ]]; then
  read -rp "NVR address as host:port (SDK port, usually 8000): " addr
  read -rp "user [admin]: " user
  user=${user:-admin}
  mkdir -p "$(dirname "$cfg")"
  install -m 600 /dev/null "$cfg"
  if command -v secret-tool >/dev/null; then
    echo "storing the password in the system keyring"
    secret-tool store --label="hikview $user@$addr" service hikview address "$addr" user "$user"
    printf '%s\n%s\n' "$addr" "$user" > "$cfg"
  else
    echo "secret-tool not found (apt install libsecret-tools) - password goes into $cfg (mode 600)"
    read -rsp "password: " pass
    echo
    printf '%s\n%s\n%s\n' "$addr" "$user" "$pass" > "$cfg"
  fi
  echo "saved $cfg"
  make -s -C "$dir" hikview
  exec "$dir/hikview" "$cfg" list
fi

[[ -r $cfg ]] || { echo "no config at $cfg - run '$0 setup' (or set HIK_CONFIG)" >&2; exit 1; }
make -s -C "$dir" hikview

case "${1:-}" in
  list)
    exec "$dir/hikview" "$cfg" list ;;
  snap)
    "$dir/hikview" "$cfg" stream "$2" sub \
      | ffmpeg -loglevel error -y -f mpeg -i - -frames:v 1 "$3" ;;
  grid)
    shift; exec "$dir/hikview" "$cfg" grid "$@" ;;
  play|recordings)
    exec "$dir/hikview" "$cfg" "$@" ;;
  export)
    (( $# == 5 )) || { sed -n '2,7p' "$0"; exit 2; }
    tmp=$(mktemp --suffix=.ps); trap 'rm -f "$tmp"' EXIT
    "$dir/hikview" "$cfg" export "$2" "$3" "$4" "$tmp"
    codec=$(ffprobe -v error -f mpeg -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$tmp")
    tag=(); [[ $codec == hevc ]] && tag=(-tag:v hvc1)  # lets QuickTime/browsers play HEVC in mp4
    ffmpeg -loglevel error -y -f mpeg -i "$tmp" -map 0:v -c copy "${tag[@]}" -movflags +faststart "$5"
    echo "saved $5" ;;
  ""|-h|--help)
    sed -n '2,7p' "$0"; exit 2 ;;
  *)
    "$dir/hikview" "$cfg" stream "$1" "${2:-}" \
      | ffplay -loglevel warning -fflags nobuffer -flags low_delay -framedrop \
          -window_title "Hikvision channel $1" -f mpeg -i - ;;
esac
