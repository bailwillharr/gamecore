#pragma once

/*
 * THIS HEADER SHOULD ONLY BE INCLUDED IN ONE TRANSLATION UNIT!
 *
 * Include this file in your application entry point file.
 * The C++ entry point is handled here along with initialisation, shutdown, etc.
 *
 * A .cpp file with nothing but the following will compile:
 * `
 * #include <gamecore/gc_game_main.h>
 * `
 */

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include "gamecore/gc_app.h"
#include "gamecore/gc_logger.h"

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        GC_CRITICAL("SDL_Init() error: {}", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    gc::App::initialise();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) { 
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) { return SDL_APP_CONTINUE; }

void SDL_AppQuit(void* appstate, SDL_AppResult result) { gc::App::shutdown(); }