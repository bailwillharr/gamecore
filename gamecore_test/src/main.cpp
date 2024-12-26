#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_asset_id.h>
#include <gamecore/gc_jobs.h>

int main()
{
    gc::App::initialise();

    gc::App::jobs().dispatch(4, 1, [](gc::JobDispatchArgs args) {
        auto data = gc::App::content().loadAsset(gc::assetIDRuntime(std::format("temple{}", args.job_index + 1)));
        GC_INFO("data size: {}", data.size());
    });
    gc::App::jobs().wait();

    for (int i = 0; i < 4; ++i) {
        auto data = gc::App::content().loadAsset(gc::assetIDRuntime(std::format("temple{}", i + 1)));
        GC_INFO("data size: {}", data.size());
    }

    gc::App::shutdown();
}