#pragma once

#include "tm_common.hpp"

#define TM    __attribute__((annotate("tm")))
#define TX    __attribute__((annotate("shared"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN  __attribute__((annotate("main"), noinline))
#define PSTATIC_REBUILD __attribute__((annotate("pstatic_rebuild")))
