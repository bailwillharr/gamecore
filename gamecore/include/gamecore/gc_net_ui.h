#pragma once

namespace gc {

class Net; // forward-dec

// Call every frame. The window is only drawn if open isn't null (see DebugUI::getWindowOpen()), but the graphs keep recording
// either way.
void renderNetUI(Net& net, bool* open);

} // namespace gc
