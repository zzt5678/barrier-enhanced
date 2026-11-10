#pragma once

#include "common.h"
#include <cstdint>

namespace barrier {

inline bool coreExitRequiresCorrection(int exitCode)
{
    return exitCode == kExitIncompatible || exitCode == kExitInputUnavailable;
}

inline bool coreGenerationMayRestart(bool hasPermanentFailure,
                                     std::uint64_t failedGeneration,
                                     std::uint64_t currentGeneration)
{
    return !hasPermanentFailure || failedGeneration != currentGeneration;
}

}
