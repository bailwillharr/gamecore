#pragma once

#include <cstdlib>

namespace gc {

[[noreturn]] void reportAssertionFailure(const char* assertion, const char* file, unsigned int line);

} // namespace gc

#if GC_DO_ASSERTS
#define GC_ASSERT(expr)                                          \
    if (expr) {                                                  \
    }                                                            \
    else {                                                       \
        ::gc::reportAssertionFailure(#expr, __FILE__, __LINE__); \
    }
#else
#define GC_ASSERT(expr) ((void)0)
#endif

/* version that doesn't log and just aborts (no logger dependency)*/
#if GC_DO_ASSERTS
#define GC_ASSERT_NOLOG(expr) \
    if (expr) {               \
    }                         \
    else {                    \
        ::std::abort();       \
    }
#else
#define GC_ASSERT_NOLOG(expr) ((void)0)
#endif
