#pragma once

#include <string>

int cmd_setup(const std::string &device, const char *sdk_zip);
int cmd_uninstall(bool purge);
