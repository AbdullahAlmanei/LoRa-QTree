
#include <stdint.h>

#if defined(ROLE_NODE)
#include "XPowersLibInterface.hpp"

// Weak default: lets us link even if the BSP doesn’t define PMU.
// If a strong PMU is provided elsewhere, it overrides this one.
XPowersLibInterface* PMU __attribute__((weak)) = nullptr;
#endif