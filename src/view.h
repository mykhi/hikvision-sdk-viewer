#pragma once

#include <ctime>
#include <vector>

#include "sdk.h"

void cmd_grid(LONG uid, std::vector<int> channels);  // empty = all online; one channel = full-quality live view
void cmd_play(LONG uid, int channel, time_t start);
