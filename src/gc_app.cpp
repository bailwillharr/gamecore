#include "gamecore/gc_app.h"

#include <memory>

#include "gamecore/gc_logger_debug.h"

namespace gc {

App::App() : m_logger(std::make_unique<LoggerDebug>()) { logger().info("constructed!"); }

App::~App() { logger().info("destructed!"); }

App& App::instance()
{
    static App app;
    return app;
}

/* cannot assert fail if logger does not exist */
Logger& App::logger() { return *m_logger; }

} // namespace gc
