#include "gamecore/gc_assert.h"

#include <cstdio>
#include <cstdlib>

#include <array>

#include "gamecore/gc_logger.h"

namespace gc {

[[noreturn]] void reportAssertionFailure(const char* assertion, const char* file, unsigned int line)
{
    std::array<char, 256> buf{};
    snprintf(buf.data(), buf.size(), "Assert fail: %s, File: %s, Line: %u\n", assertion, file, line);
    Logger::instance().critical(buf.data());
    std::abort();
}

} // namespace gc
