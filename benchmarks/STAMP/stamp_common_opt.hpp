#pragma once
// Minimal: just includes stamp_common.hpp (no tm_api.hpp)
#include "stamp_common.hpp"
#define TM_LOCAL __attribute__((annotate("tm_local")))
