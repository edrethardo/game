#pragma once

#include "core/log.h"
#include <cstdlib>

#ifdef ENGINE_DEBUG
    #define ENGINE_ASSERT(cond, msg)                                        \
        do {                                                                \
            if (!(cond)) {                                                  \
                LOG_ERROR("ASSERTION FAILED: %s  |  %s", #cond, msg);      \
                std::abort();                                               \
            }                                                               \
        } while (0)
#else
    #define ENGINE_ASSERT(cond, msg) ((void)0)
#endif
