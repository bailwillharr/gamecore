#include "gamecore/gc_prefab.h"

#include <cstring>

#include <vector>

#include <gcpak/gcpak.h>
#include <gcpak/gcpak_prefab.h>

#include "gclog/gclog.h"
#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_transform_system.h"
#include "gamecore/gc_world.h"

namespace gc {

// parent index, then the transform
static constexpr size_t ENTITY_DECLARATION_SIZE = sizeof(uint32_t) + TransformComponent::getSerialisedSize();

Entity loadPrefab(std::span<const uint8_t> data, World& world, Entity prefab_parent, std::vector<Entity>* entities_out)
{
    std::vector<Entity> entities{}; // indexed by the entity's index in the prefab
    std::vector<Entity> roots{};

    // Nothing in the data is trusted. If something is wrong with it, remove what was created so far and give up.
    const auto fail = [&]() {
        for (Entity root : roots) {
            world.deleteEntity(root); // also deletes its children
        }
        return ENTITY_NONE;
    };

    ByteReader reader(data);
    while (reader.remaining() > 0) {
        if (reader.remaining() < gcpak::PREFAB_COMPONENT_HEADER_SIZE) {
            GC_ERROR("Corrupt prefab: data ends in the middle of a component declaration");
            return fail();
        }
        const Name component_name(reader.readU32());
        const size_t size = reader.readU32();
        if (size > reader.remaining()) {
            GC_ERROR("Corrupt prefab: {} is {} bytes but only {} bytes of data remain", component_name, size, reader.remaining());
            return fail();
        }
        // components only get to read their own data
        ByteReader component_reader(data.subspan(reader.pos(), size));
        reader.skip(size);

        if (component_name == TransformComponent::NAME) {
            // a new entity
            if (size != ENTITY_DECLARATION_SIZE) {
                GC_ERROR("Corrupt prefab: entity declaration is {} bytes, expected {}", size, ENTITY_DECLARATION_SIZE);
                return fail();
            }
            const uint32_t parent_index = component_reader.readU32();
            TransformComponent transform{};
            transform.deserialise(component_reader);

            Entity parent = prefab_parent;
            if (parent_index != gcpak::PREFAB_NO_PARENT) {
                if (parent_index >= entities.size()) {
                    GC_ERROR("Corrupt prefab: entity {} has parent {}, which hasn't been declared yet", entities.size(), parent_index);
                    return fail();
                }
                parent = entities[parent_index];
            }

            const Entity entity = world.createEntity(transform.name, parent, transform.getPosition(), transform.getRotation(), transform.getScale());
            entities.push_back(entity);
            if (parent_index == gcpak::PREFAB_NO_PARENT) {
                roots.push_back(entity);
            }
            continue;
        }

        if (entities.empty()) {
            GC_ERROR("Corrupt prefab: {} appears before the first entity", component_name);
            return fail();
        }

        const auto expected_size = world.getComponentSerialisedSize(component_name);
        if (!expected_size) {
            // Not an error. For example, a dedicated server has no use for components that are only for rendering.
            if (world.isComponentRegistered(component_name)) {
                GC_WARN("Skipping {} in prefab as it isn't serialisable", component_name);
            }
            else {
                GC_WARN("Skipping {} in prefab as no component with that name is registered", component_name);
            }
            continue;
        }
        if (size != *expected_size) {
            GC_ERROR("Corrupt prefab: {} is {} bytes, expected {}. Was the prefab made by a different version of the game?", component_name, size,
                     *expected_size);
            return fail();
        }
        world.deserialiseComponent(entities.back(), component_name, component_reader);
    }

    if (roots.empty()) {
        GC_ERROR("Prefab is empty");
        return ENTITY_NONE;
    }

    if (entities_out) {
        entities_out->insert(entities_out->end(), entities.begin(), entities.end());
    }
    return roots.front();
}

Entity loadPrefab(const Content& content, Name prefab_name, World& world, Entity prefab_parent, std::vector<Entity>* entities_out)
{
    const AssetView asset = content.findAsset(prefab_name);
    if (asset.data.empty()) {
        return ENTITY_NONE; // findAsset() has logged the error
    }
    if (asset.type != gcpak::GcpakAssetType::PREFAB) {
        GC_ERROR("Asset {} is not a prefab", prefab_name);
        return ENTITY_NONE;
    }
    const Entity root = loadPrefab(asset.data, world, prefab_parent, entities_out);
    if (root == ENTITY_NONE) {
        GC_ERROR("Failed to load prefab {}", prefab_name);
    }
    return root;
}

static void saveEntityRecursively(World& world, const TransformSystem& transform_system, PrefabWriter& writer, Entity entity, uint32_t parent_index)
{
    const TransformComponent* const transform = world.getComponent<TransformComponent>(entity);
    GC_ASSERT(transform);
    const uint32_t index = writer.beginEntity(transform->name, parent_index, transform->getPosition(), transform->getRotation(), transform->getScale());

    for (const Name component_name : world.getComponentList(entity)) {
        if (component_name == TransformComponent::NAME) {
            continue;
        }
        if (const auto size = world.getComponentSerialisedSize(component_name); size) {
            ByteWriter component_writer(writer.addComponentDeclaration(component_name, *size));
            world.serialiseComponent(entity, component_name, component_writer);
        }
        else {
            GC_TRACE("Leaving {} of entity {} out of prefab as it isn't serialisable", component_name, transform->name);
        }
    }

    for (const Entity child : transform_system.getChildren(entity)) {
        saveEntityRecursively(world, transform_system, writer, child, index);
    }
}

std::vector<uint8_t> savePrefab(World& world, Entity root)
{
    PrefabWriter writer{};
    saveEntityRecursively(world, world.getSystem<TransformSystem>(), writer, root, gcpak::PREFAB_NO_PARENT);
    const auto data = writer.getData();
    return std::vector<uint8_t>(data.begin(), data.end());
}

std::vector<uint8_t> saveWorldAsPrefab(World& world)
{
    // Nothing is added to the world while saving, so the references passed to the callback stay valid.
    PrefabWriter writer{};
    const TransformSystem& transform_system = world.getSystem<TransformSystem>();
    world.forEach<TransformComponent>([&](Entity entity, TransformComponent& transform) {
        if (transform.getParent() == ENTITY_NONE) {
            saveEntityRecursively(world, transform_system, writer, entity, gcpak::PREFAB_NO_PARENT);
        }
    });
    const auto data = writer.getData();
    return std::vector<uint8_t>(data.begin(), data.end());
}

uint32_t PrefabWriter::beginEntity(Name name, uint32_t parent_index, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
{
    GC_ASSERT(parent_index == gcpak::PREFAB_NO_PARENT || parent_index < m_entity_count);

    TransformComponent transform{};
    transform.name = name;
    transform.setPosition(position);
    transform.setRotation(rotation);
    transform.setScale(scale);

    ByteWriter writer(addComponentDeclaration(TransformComponent::NAME, ENTITY_DECLARATION_SIZE));
    writer.writeU32(parent_index);
    transform.serialise(writer);
    GC_ASSERT(writer.remaining() == 0);

    return m_entity_count++;
}

std::span<uint8_t> PrefabWriter::addComponentDeclaration(Name component_name, size_t size)
{
    // only an entity declaration can come first
    GC_ASSERT(m_entity_count > 0 || component_name == TransformComponent::NAME);

    const size_t offset = m_data.size();
    m_data.resize(offset + gcpak::PREFAB_COMPONENT_HEADER_SIZE + size);

    ByteWriter writer(std::span<uint8_t>(m_data).subspan(offset));
    writer.writeU32(component_name.getHash());
    writer.writeU32(static_cast<uint32_t>(size));

    return std::span<uint8_t>(m_data).subspan(offset + gcpak::PREFAB_COMPONENT_HEADER_SIZE, size);
}

uint32_t PrefabWriter::getEntityCount() const { return m_entity_count; }

std::span<const uint8_t> PrefabWriter::getData() const { return m_data; }

} // namespace gc
