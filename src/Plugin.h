#pragma once
#include <filesystem>
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include "Config.h"

namespace RED4ext::v1 { struct Sdk; }

namespace redscope {

const std::filesystem::path& PluginDir();
const Config& GetConfig();

RED4ext::v1::PluginHandle PluginHandle();
const RED4ext::v1::Sdk*   Sdk();

}
