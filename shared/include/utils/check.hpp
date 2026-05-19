// This contains the implementation for the CHECK and DCHECK
// flags so that this code is clean like the chromium codebase ;)

#pragma once

#include <cstdlib>
#include <iostream>

#define ZAPSHARE_CHECK_IMPL(condition, kind)                                  \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::cerr << kind << " failed: " #condition << " at " << __FILE__ \
                      << ":" << __LINE__ << std::endl;                        \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

#ifndef NDEBUG
#define DCHECK(condition) ZAPSHARE_CHECK_IMPL(condition, "DCHECK")
#else
#define DCHECK(condition) \
    do {                  \
    } while (false)
#endif

#define CHECK(condition) ZAPSHARE_CHECK_IMPL(condition, "CHECK")