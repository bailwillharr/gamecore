#pragma once

namespace gc {

[[noreturn]] void reportAssertionFailure(const char* assertion, const char* file, unsigned int line);

} // namespace gc

#if GC_DEV_BUILD
#define GC_ASSERT(expr)                                          \
    if (expr) {                                                  \
    }                                                            \
    else {                                                       \
        ::gc::reportAssertionFailure(#expr, __FILE__, __LINE__); \
    }
#else
#define GC_ASSERT(expr) ((void)0)
#endif
