#pragma once

// Keeps entities in sync between a server and its clients. Sits on top of gc::Net.
//
// Summary:
//  - The server (or the local game when not connected to anything) is the authority. Only the authority spawns and despawns
//    replicated entities, using ReplicationSystem::spawn() and despawn().
//  - Entities are spawned from archetypes. An archetype is a callback, registered by the game on every host, that adds the
//    entity's components. It is told whether the local host has authority and whether it owns the entity, so the same archetype
//    can build a full simulation entity on the server, a camera on the owning client, and just a model everywhere else.
//  - Component types registered with registerComponent() are kept in sync. Each one is either controlled by the server, or by the
//    client that owns the entity (ReplicationAuthority). The server relays owner controlled components to the other clients.
//  - The transform is always replicated. On clients, transforms they don't control are interpolated between received states.
//  - Spawns and despawns are sent reliably. Component state is sent unreliably at a fixed tick rate, and only when it has changed.
//    Each host acknowledges the state messages it receives, so a lost change is sent again until it is known to have arrived.
//  - Anything else (requests, one-off effects) should be sent as normal events with Net::postEvent(), using NetEntityId
//    to refer to entities.
//
// See notes/replication.txt for more.

#include <cstdint>

#include <array>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_net_common.h"
#include "gamecore/gc_world.h"

namespace gc {

class Net; // forward-dec

// Identifies a replicated entity on every host. Entity handles are only meaningful locally.
using NetEntityId = uint32_t;

constexpr NetEntityId NET_ENTITY_NONE = 0;

enum class ReplicationRole : uint8_t {
    NONE,    // before the first update
    OFFLINE, // not connected to anything. The local game is the authority
    SERVER,  // the authority, with clients
    CLIENT,  // connecting or connected to a server
};

// Who decides the value of a replicated component
enum class ReplicationAuthority : uint8_t {
    SERVER, // always the server
    OWNER,  // the client that owns the entity (the server, if the entity isn't owned by a client)
};

enum class ReplicationEventKind : uint8_t {
    AUTHORITY_STARTED, // The local host has become the authority and there are no replicated entities. Time to spawn the world.
    PEER_JOINED,       // authority only: a client connected
    PEER_LEFT,         // authority only: a client disconnected. The entities it owned have already been despawned
};

// Added to every replicated entity. Don't add or modify this yourself.
struct ReplicatedComponent {
    static constexpr auto NAME = Name::createConstexpr("ReplicatedComponent");

    NetEntityId net_id{NET_ENTITY_NONE};
    Name archetype{};
    NetPeerId owner{NET_PEER_NONE}; // NET_PEER_NONE if the entity isn't owned by a particular host
};

struct ReplicationSpawnInfo {
    NetEntityId net_id{};
    Name archetype{};
    NetPeerId owner{};
    bool has_authority{}; // the local host is the server (or offline)
    bool locally_owned{}; // the local host is the entity's owner
};

struct ReplicationArchetype {
    // Called on every host after the entity is created (with a TransformComponent and ReplicatedComponent), to add the rest of its
    // components. Replicated components that should be kept in sync must be added here on every host.
    std::function<void(World& world, Entity entity, const ReplicationSpawnInfo& info)> on_spawn{};

    ReplicationAuthority transform_authority{ReplicationAuthority::OWNER};

    // Clients smooth the transform between received states instead of snapping to the latest one
    bool interpolate_transform{true};
};

struct ReplicationEvent {
    ReplicationEventKind kind{};
    NetPeerId peer{NET_PEER_NONE};
};

struct ReplicationLinkStats {
    NetPeerId peer{};
    uint32_t entities{};      // entities the remote host knows about
    uint32_t pending{};       // component states that the remote host doesn't have the latest version of yet
    uint64_t messages_sent{}; // state messages
    uint64_t messages_received{};
    uint64_t bytes_sent{};
    uint64_t bytes_received{};
    uint64_t states_sent{}; // component states
    uint64_t states_resent{};
    uint64_t states_received{};
    // clients only, about the state messages from the server:
    float jitter{};              // seconds. How much the time taken for them to arrive varies
    float loss{};                // fraction (0-1) that didn't arrive (or arrived out of order)
    float interpolation_delay{}; // seconds. How far in the past interpolated transforms are currently shown
};

// A component can be replicated if it has a NAME, and:
//   void serialise(ByteWriter& writer) const;   // must not write more than ReplicationSystem::MAX_COMPONENT_SIZE bytes
//   void deserialise(ByteReader& reader);       // must read exactly what serialise() writes
// Only serialise the fields that need to be in sync. Never trust the values read by deserialise() to be sensible.
template <typename T>
concept ReplicableComponent = ValidComponent<T> && requires(const T& component, T& mutable_component, ByteWriter& writer, ByteReader& reader) {
    { T::NAME } -> std::convertible_to<Name>;
    component.serialise(writer);
    mutable_component.deserialise(reader);
};

class ReplicationSystem : public System {
public:
    static constexpr auto NAME = Name::createConstexpr("ReplicationSystem");

    static constexpr size_t MAX_COMPONENT_TYPES = 16; // including the transform
    static constexpr size_t MAX_COMPONENT_SIZE = 128; // serialised

private:
    struct ComponentType {
        Name name{};
        ReplicationAuthority authority{};
        bool (*serialise)(World&, Entity, ByteWriter&){};   // returns false if the entity doesn't have the component
        bool (*deserialise)(World&, Entity, ByteReader&){}; // returns false if the entity doesn't have the component
    };

    // The most recently serialised state of a component, and how many times it has changed
    struct ComponentState {
        std::array<uint8_t, MAX_COMPONENT_SIZE> bytes{};
        uint8_t size{};
        uint32_t version{}; // 0 if the entity doesn't have the component
    };

    struct TransformSample {
        double time{}; // remote host's clock, seconds
        glm::vec3 position{};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    struct EntityRecord {
        Entity entity{ENTITY_NONE};
        NetEntityId net_id{};
        Name archetype{};
        NetPeerId owner{};
        ReplicationAuthority transform_authority{};
        bool interpolate_transform{};
        std::array<ComponentState, MAX_COMPONENT_TYPES> states{};
        std::array<uint32_t, MAX_COMPONENT_TYPES> applied_seq{}; // sequence number of the state message each component was last set by
        // Oldest first. Has to reach further back than the interpolation delay, or there is nothing to interpolate from.
        std::array<TransformSample, 32> samples{};
        uint32_t sample_count{};
    };

    // What the remote host on a link is known to have of one component of one entity
    struct ItemState {
        uint32_t sent_version{};
        uint32_t acked_version{};
        uint32_t sent_seq{};
        uint64_t sent_tick{};
        bool in_flight{}; // sent, and not yet known to have arrived or been lost
    };

    struct Link {
        NetPeerId peer{};
        uint32_t next_seq{1};
        uint32_t received_seq{0};  // highest state message sequence number seen from the remote host
        uint32_t received_bits{0}; // bit N set if (received_seq - N) was received and fully applied
        bool ack_pending{false};   // something has been received since the last state message was sent
        std::unordered_map<NetEntityId, std::array<ItemState, MAX_COMPONENT_TYPES>> items{}; // the entities the remote host knows about
        bool has_time_offset{false};
        double time_offset{};         // remote clock - local clock
        double jitter{};              // average difference between time_offset and what each state message says it is
        double loss{};                // fraction of state messages that were missing when a newer one arrived
        double interpolation_delay{}; // what is currently used, which follows what jitter and loss call for
        ReplicationLinkStats stats{};
    };

    static constexpr uint32_t TRANSFORM_INDEX = 0;

    Net& m_net;

    std::vector<ComponentType> m_component_types{};
    std::unordered_map<Name, ReplicationArchetype> m_archetypes{};

    ReplicationRole m_role{ReplicationRole::NONE};
    std::unordered_map<NetEntityId, EntityRecord> m_entities{};
    std::unordered_map<NetPeerId, Link> m_links{};
    std::vector<NetEntityId> m_pending_despawns{};
    std::vector<ReplicationEvent> m_events{};
    NetEntityId m_next_net_id{1};

    double m_time{}; // seconds since the system was created
    double m_delta_time{};
    double m_tick_accumulator{};
    uint64_t m_tick_count{};
    float m_tick_rate{30.0f};
    float m_interpolation_delay{0.1f}; // the minimum
    float m_teleport_distance{10.0f};
    bool m_show_debug_ui{true};

public:
    ReplicationSystem(World& world, Net& net);

    void onUpdate(FrameState& frame_state) override;

    // Call on every host, in the same order, before any entities are spawned.
    // The component must already be registered with the World.
    template <ReplicableComponent T>
    void registerComponent(ReplicationAuthority authority)
    {
        ComponentType type{};
        type.name = T::NAME;
        type.authority = authority;
        type.serialise = [](World& world, Entity entity, ByteWriter& writer) {
            const T* const component = world.getComponent<T>(entity);
            if (component) {
                component->serialise(writer);
            }
            return component != nullptr;
        };
        type.deserialise = [](World& world, Entity entity, ByteReader& reader) {
            T* const component = world.getComponent<T>(entity);
            if (component) {
                component->deserialise(reader);
            }
            return component != nullptr;
        };
        addComponentType(type);
    }

    // Call on every host before any entities are spawned.
    void registerArchetype(Name name, ReplicationArchetype archetype);

    ReplicationRole getRole() const;

    // True if the local host is the one that spawns entities (it's the server, or offline)
    bool hasAuthority() const;

    // NET_PEER_SERVER if the local host has authority
    NetPeerId getLocalPeerId() const;

    // What happened since the last update. Systems registered after this one see each event exactly once.
    std::span<const ReplicationEvent> getEvents() const;

    // Authority only. Creates an entity from an archetype on every host. Returns ENTITY_NONE on failure.
    // Component values set straight after spawning are part of the entity's initial state on other hosts.
    Entity spawn(Name archetype, NetPeerId owner = NET_PEER_NONE, const glm::vec3& position = glm::vec3{0.0f, 0.0f, 0.0f},
                 const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f});

    // Authority only. Deletes a replicated entity on every host.
    // Deleting a replicated entity with World::deleteEntity() has the same effect, a little later.
    void despawn(Entity entity);

    // returns ENTITY_NONE if there is no such entity
    Entity findEntity(NetEntityId net_id) const;

    // returns NET_ENTITY_NONE if the entity isn't replicated
    NetEntityId getNetId(Entity entity) const;

    bool isLocallyOwned(Entity entity) const;

    // How many times per second state is sent
    void setTickRate(float tick_rate);
    float getTickRate() const;

    // The least that clients show interpolated transforms in the past by (seconds).
    // There needs to be a newer state to interpolate towards, so the delay actually used is longer than this if the connection
    // needs it: a few ticks, plus more the less regularly state messages arrive (see ReplicationLinkStats).
    void setInterpolationDelay(float seconds);
    float getInterpolationDelay() const;

    void setShowDebugUI(bool show);

    std::vector<ReplicationLinkStats> getLinkStats() const;
    size_t getEntityCount() const;

    // A hash of the current value of every replicated component of every replicated entity.
    // Once nothing has changed for a little while, this is the same on every host. If it isn't, something is out of sync.
    uint32_t computeStateDigest() const;

private:
    void addComponentType(const ComponentType& type);
    uint32_t computeProtocolHash() const;

    void updateRole();
    void becomeAuthority();
    void becomeClient();
    void deleteAllEntities();

    void handleNetEvent(const NetEvent& ev);
    void onPeerJoined(NetPeerId peer);
    void onPeerLeft(NetPeerId peer);
    void onWelcomeMessage(const NetEvent& ev);
    void onSpawnMessage(const NetEvent& ev);
    void onDespawnMessage(const NetEvent& ev);
    void onStateMessage(const NetEvent& ev);

    EntityRecord& createRecord(Entity entity, NetEntityId net_id, Name archetype, NetPeerId owner);
    void destroyRecord(NetEntityId net_id, bool delete_entity);
    bool isRecordAlive(const EntityRecord& record) const;

    // The host that decides the value of a component
    NetPeerId getAuthorityPeer(const EntityRecord& record, uint32_t component_index) const;
    bool shouldSend(const EntityRecord& record, uint32_t component_index, NetPeerId to_peer) const;
    bool shouldAccept(const EntityRecord& record, uint32_t component_index, NetPeerId from_peer) const;

    size_t serialiseComponent(const EntityRecord& record, uint32_t component_index, std::span<uint8_t> out) const;
    enum class ApplyMode {
        INITIAL, // came with the entity's spawn. Applied immediately, never interpolated
        UPDATE,  // newer than what the component was last set to
        STALE,   // older than what the component was last set to. Only of use as something to interpolate through
    };
    void applyComponent(EntityRecord& record, uint32_t component_index, std::span<const uint8_t> data, ApplyMode mode, double remote_time);
    void pushTransformSample(EntityRecord& record, TransformSample sample, bool is_latest);
    void interpolateTransforms();

    void tick();
    void removeDeletedEntities();
    void captureStates();
    void sendSpawns(Link& link);
    void sendDespawns();
    void processAcks(Link& link, uint32_t ack_seq, uint32_t ack_bits);
    void sendStates(Link& link);

    void renderDebugUI();
};

} // namespace gc
