#pragma once

#include "core/types.h"

namespace Clock {
    void init();
    void update();

    f64 getDeltaSeconds();
    f64 getElapsedSeconds();
    u64 getFrameCount();
}
