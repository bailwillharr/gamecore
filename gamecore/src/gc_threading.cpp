#include "gamecore/gc_threading.h"

#include <thread>

namespace gc {

/* Initialised by App::initialise() */
bool isMainThread()
{
    static const auto main_thread_id = std::this_thread::get_id();
    return std::this_thread::get_id() == main_thread_id;
}

} // namespace gc