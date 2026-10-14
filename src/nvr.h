#pragma once

#include <ctime>
#include <vector>

#include "sdk.h"

std::vector<int> online_channels(LONG uid);
void cmd_list(LONG uid, const NET_DVR_DEVICEINFO_V40 &info);
void cmd_recordings(LONG uid, int channel, time_t day);
int cmd_export(LONG uid, int channel, time_t from, time_t to, const char *out);  // .ps as-is, else remux via ffmpeg
int cmd_snap(LONG uid, int channel, const char *out);
void cmd_stream(LONG uid, int channel, bool sub);
