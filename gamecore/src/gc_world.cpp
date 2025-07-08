#include "gamecore/gc_world.h"

#include "gamecore/gc_logger.h"

namespace gc {

World::World() { GC_TRACE("Initialised World"); }

World::~World() { GC_TRACE("Destroying World..."); }

Entity World::createEntity()
{
    Entity id = m_next_entity_id++;

    m_signatures.emplace(id, std::bitset<MAX_COMPONENTS>{});

    // TODO: currently there isn't even a transform component therefore entity has no tag

    return id;
}

size_t World::getComponentSignaturePosition(size_t hash) { return m_component_signature_positions.at(hash); }

void World::update(float ts)
{
    (void)ts;
    // TODO
}

} // namespace gc