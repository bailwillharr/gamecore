#pragma once

#include <optional>

namespace gc {
class App; // forward-dec
}

struct Options {
    std::optional<int> render_sync_mode{};
};

void buildAndStartGame(gc::App& app, Options options);