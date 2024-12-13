#include "gamecore/gc_abort.h"

#include <cstdlib>

#include "gamecore/gc_logger.h"

namespace gc {

[[noreturn]] void abortGame(std::string_view msg)
{
    // test
    Logger::instance().critical(msg);
    std::abort();
}

} // namespace gc