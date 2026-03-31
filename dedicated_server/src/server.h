#pragma once

#include <string>
#include <cstdint>

namespace gc {
class App; // forward-dec
}

struct Options {
    std::string bind_address{};
    uint16_t bind_port{};
};

void buildAndStartServer(gc::App& app, Options options);