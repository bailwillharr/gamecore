#include "gamecore/gc_ecs.h"

namespace gc {

System::System(std::bitset<MAX_COMPONENTS> signature) : m_signature(signature) {}

} // namespace gc
