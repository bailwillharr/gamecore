#include <gamecore/gc_app.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_asset_id.h>
#include <gamecore/gc_stopwatch.h>

int main()
{

    auto lifetime_stopwatch = gc::tick("App lifetime");

    // initialise gamecore
    gc::App::initialise();

    gc::Jobs& jobs = gc::App::instance().jobs();

    gc::Logger& logger = gc::Logger::instance();

    logger.info(std::format("CRC32 of 'test' is: {:8X}", gc::assetID("test")));

    gc::App::shutdown();

    gc::tock(lifetime_stopwatch);
}