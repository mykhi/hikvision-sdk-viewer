#pragma once

#include <string>
#include <vector>

#include "sdk.h"

// A device config is a text file: line 1 "host:port", line 2 user, optional line 3 password (file must be 600).
struct Device {
    std::string name;  // empty when loaded with -c FILE
    std::string path;
    std::string address;
    std::string user;
    std::string password;
};

std::string config_dir();
std::string device_path(const std::string &name);
std::vector<std::string> device_names();
bool load_device(const std::string &path, Device &d);

// Password order: config file line 3, $HIK_PASSWORD, system keyring (secret-tool), terminal prompt.
LONG login(Device &d, NET_DVR_DEVICEINFO_V40 *info);

bool keyring_available();
bool keyring_store(const std::string &address, const std::string &user, const std::string &password);
void keyring_clear(const std::string &address, const std::string &user);
