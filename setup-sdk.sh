#!/usr/bin/env bash
# Usage: setup-sdk.sh <EN-HCNetSDK..._linux64.zip>
# Unpacks the Hikvision Device Network SDK (Linux 64-bit) you downloaded from hikvision.com and builds hikview.
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
zip=${1:?usage: setup-sdk.sh <EN-HCNetSDK..._linux64.zip>}

rm -rf "$dir/sdk" "$dir/lib"
unzip -q "$zip" -d "$dir/sdk"
header=$(find "$dir/sdk" -path '*/incEn/HCNetSDK.h' | head -n1)
[[ -n $header && -f $(dirname "$(dirname "$header")")/lib/libhcnetsdk.so ]] || {
  echo "this doesn't look like the Linux 64-bit Device Network SDK (no incEn/HCNetSDK.h or lib/libhcnetsdk.so)" >&2
  exit 1
}
make -C "$dir"
echo "built $dir/hikview"
