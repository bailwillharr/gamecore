#include "gamecore/gc_assert.h"

#include <cstdlib>

#include "gamecore/gc_logger.h"

namespace gc {

[[noreturn]] void reportAssertionFailure(const char* assertion, const char* file, unsigned int line)
{
    GC_CRITICAL("Assert fail: {}, File: {}, Line: {}", assertion, file, line);
    std::abort();
}

} // namespace gc
