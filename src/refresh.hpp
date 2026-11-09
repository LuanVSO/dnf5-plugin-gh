#pragma once

#include "store.hpp"

#include <cstdint>
#include <string>

namespace ghplugin {

constexpr std::int64_t REFRESH_INTERVAL = 24 * 60 * 60;

bool refresh_due(const Tracked & tracked, std::int64_t now);
std::string refresh_repository(Store & store, Client & client, Tracked & tracked, const std::string & arch);

}  // namespace ghplugin
