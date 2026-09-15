#pragma once
#include "topology/DeviceTopology.h"
#include <set>
class Config;
namespace weave { namespace topology {
Topology topologyFromConfig(const Config&, const std::string& primary,
                            const std::set<std::string>& admitted,
                            std::uint64_t epoch, std::uint64_t revision);
} }
