#pragma once

#include <csignal>
#include <ctime>
#include <string>
#include <vector>

#include "sdk.h"

extern volatile sig_atomic_t g_stop;

long long now_ms();
NET_DVR_TIME to_dvr(time_t t);  // NVR clock is local time (NTP-synced, same as this PC)
time_t from_dvr(const NET_DVR_TIME &d);
std::string fmt(time_t t, const char *f = "%Y-%m-%d %H:%M:%S");
// "YYYY-MM-DD HH:MM[:SS]", "HH:MM[:SS]" (today), "YYYY-MM-DD", or relative "-30s" "-10m" "-2h" "-1d".
time_t parse_time(const char *s);

// Runs a command without a shell. run() inherits stdout/stderr and returns the exit status (-1 if it
// couldn't start); `input`, if given, is written to its stdin. run_line() returns the first output line.
int run(std::vector<const char *> argv, const std::string *input = nullptr);
std::string run_line(std::vector<const char *> argv);
bool have_command(const char *name);

// Reads a line from the terminal (hidden for secrets); "" if there is no terminal.
std::string ask(const std::string &question, bool secret = false);
