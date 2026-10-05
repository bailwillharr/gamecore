#include "gamecore/gc_replication.h"

#include <cmath>
#include <cstring>

#include <algorithm>
#include <string>

#include <glm/geometric.hpp>

#include <imgui.h>

#include <tracy/Tracy.hpp>

#include <gclog/gclog.h>

#include "gamecore/gc_app.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_debug_ui.h"
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_net.h"
#include "gamecore/gc_transform_component.h"

namespace gc {

// Messages, sent as net events. All integers are little endian.
//
// repl_welcome (server -> client, reliable). Sent when a client joins.
//   uint32_t protocol_hash     describes the registered components and archetypes, which must be the same on both hosts
//
// repl_spawn (server -> client, reliable)
//   uint32_t net_id
//   uint32_t archetype         (Name hash)
//   uint32_t owner             (NetPeerId)
//   uint32_t time_ms           the server's clock when the component states below were captured
//   uint8_t component_count
//   component_count times:
//     uint8_t component_index  (0 is the transform, the rest are in registration order)
//     uint8_t size
//     uint8_t data[size]
//
// repl_despawn (server -> client, reliable)
//   uint32_t net_id
//
// repl_state (both directions, unreliable)
//   uint32_t seq               sequence number of this message. Starts at 1
//   uint32_t time_ms           the sender's clock
//   uint32_t ack_seq           the highest sequence number the sender has seen from the receiver
//   uint32_t ack_bits          bit N is set if (ack_seq - N) was received and everything in it was applied
//   until the end of the message:
//     uint32_t net_id
//     uint8_t component_count
//     component_count times: as in repl_spawn
static constexpr Name MESSAGE_WELCOME = Name::createConstexpr("repl_welcome");
static constexpr Name MESSAGE_SPAWN = Name::createConstexpr("repl_spawn");
static constexpr Name MESSAGE_DESPAWN = Name::createConstexpr("repl_despawn");
static constexpr Name MESSAGE_STATE = Name::createConstexpr("repl_state");

static constexpr size_t STATE_HEADER_SIZE = 4 * sizeof(uint32_t);
static constexpr size_t ENTRY_HEADER_SIZE = sizeof(uint32_t) + sizeof(uint8_t);
static constexpr size_t COMPONENT_HEADER_SIZE = 2 * sizeof(uint8_t);

// Leaves room for the event's type and the transport's headers in a single packet
static constexpr size_t STATE_MESSAGE_BUDGET = 1100;
static constexpr int MAX_STATE_MESSAGES_PER_TICK = 4;

// A state that hasn't been acknowledged or reported lost after this many ticks is sent again
static constexpr uint64_t RESEND_TICKS = 8;

static constexpr uint8_t TRANSFORM_FLAG_HAS_SCALE = 1 << 0;
static constexpr size_t TRANSFORM_SIZE = sizeof(uint8_t) + 7 * sizeof(float);
static constexpr size_t TRANSFORM_SIZE_WITH_SCALE = TRANSFORM_SIZE + 3 * sizeof(float);

static_assert(STATE_MESSAGE_BUDGET + sizeof(uint32_t) <= NetConnection::MAX_UNRELIABLE_SIZE);
static_assert(STATE_HEADER_SIZE + ENTRY_HEADER_SIZE + COMPONENT_HEADER_SIZE + ReplicationSystem::MAX_COMPONENT_SIZE <= STATE_MESSAGE_BUDGET);
static_assert(ReplicationSystem::MAX_COMPONENT_SIZE <= UINT8_MAX);
static_assert(TRANSFORM_SIZE_WITH_SCALE <= ReplicationSystem::MAX_COMPONENT_SIZE);

static const char* roleString(ReplicationRole role)
{
    switch (role) {
    case ReplicationRole::NONE:
        return "none";
    case ReplicationRole::OFFLINE:
        return "offline";
    case ReplicationRole::SERVER:
        return "server";
    case ReplicationRole::CLIENT:
        return "client";
    }
    return "(invalid)";
}

static bool isFinite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

ReplicationSystem::ReplicationSystem(World& world, Net& net) : System(world), m_net(net)
{
    m_world.registerComponent<ReplicatedComponent, ComponentArrayType::SPARSE>();

    // The transform is handled by this system directly as it needs interpolating
    ComponentType transform_type{};
    transform_type.name = TransformComponent::NAME;
    m_component_types.push_back(transform_type);
}

void ReplicationSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    m_events.clear();
    m_time += frame_state.delta_time;
    m_delta_time = frame_state.delta_time;

    updateRole();

    for (const NetEvent& ev : frame_state.net_events) {
        handleNetEvent(ev);
    }

    if (m_role == ReplicationRole::CLIENT) {
        interpolateTransforms();
    }

    // State is sent at a fixed rate, independent of the frame rate
    const double tick_interval = 1.0 / static_cast<double>(m_tick_rate);
    m_tick_accumulator += frame_state.delta_time;
    if (m_tick_accumulator >= tick_interval) {
        m_tick_accumulator = std::fmod(m_tick_accumulator, tick_interval);
        tick();
    }

    // There is only an ImGui context if the app has a window. Like the other debug windows, it's only shown on request
    // (F10, then the Windows menu)
    if (m_show_debug_ui && ImGui::GetCurrentContext()) {
        if (bool* const open = App::instance().debugUI().getWindowOpen("Replication")) {
            renderDebugUI(open);
        }
    }
}

void ReplicationSystem::registerArchetype(Name name, ReplicationArchetype archetype)
{
    GC_ASSERT(m_entities.empty());
    if (!m_archetypes.try_emplace(name, std::move(archetype)).second) {
        GC_ERROR("Replication archetype {} registered twice", name);
    }
}

ReplicationRole ReplicationSystem::getRole() const { return m_role; }

bool ReplicationSystem::hasAuthority() const { return m_role == ReplicationRole::OFFLINE || m_role == ReplicationRole::SERVER; }

NetPeerId ReplicationSystem::getLocalPeerId() const { return hasAuthority() ? NET_PEER_SERVER : m_net.getLocalPeerId(); }

std::span<const ReplicationEvent> ReplicationSystem::getEvents() const { return m_events; }

Entity ReplicationSystem::spawn(Name archetype, NetPeerId owner, const glm::vec3& position, const glm::quat& rotation)
{
    if (!hasAuthority()) {
        GC_ERROR("Only the authority can spawn replicated entities ({})", archetype);
        return ENTITY_NONE;
    }
    if (!m_archetypes.contains(archetype)) {
        GC_ERROR("Cannot spawn unknown replication archetype {}", archetype);
        return ENTITY_NONE;
    }

    const Entity entity = m_world.createEntity(archetype, ENTITY_NONE, position, rotation);
    createRecord(entity, m_next_net_id++, archetype, owner);
    // Other hosts are told at the next tick, so that anything the caller sets up now is included in the initial state
    return entity;
}

void ReplicationSystem::despawn(Entity entity)
{
    const NetEntityId net_id = getNetId(entity);
    if (net_id == NET_ENTITY_NONE) {
        GC_ERROR("Cannot despawn entity {} as it isn't replicated", entity);
        return;
    }
    if (!hasAuthority()) {
        GC_ERROR("Only the authority can despawn replicated entities");
        return;
    }
    destroyRecord(net_id, true);
    m_pending_despawns.push_back(net_id);
}

Entity ReplicationSystem::findEntity(NetEntityId net_id) const
{
    const auto it = m_entities.find(net_id);
    if (it == m_entities.end() || !isRecordAlive(it->second)) {
        return ENTITY_NONE;
    }
    return it->second.entity;
}

NetEntityId ReplicationSystem::getNetId(Entity entity) const
{
    const ReplicatedComponent* const replicated = m_world.getComponent<ReplicatedComponent>(entity);
    return replicated ? replicated->net_id : NET_ENTITY_NONE;
}

bool ReplicationSystem::isLocallyOwned(Entity entity) const
{
    const ReplicatedComponent* const replicated = m_world.getComponent<ReplicatedComponent>(entity);
    return replicated && replicated->owner != NET_PEER_NONE && replicated->owner == getLocalPeerId();
}

void ReplicationSystem::setTickRate(float tick_rate) { m_tick_rate = std::clamp(tick_rate, 1.0f, 240.0f); }

float ReplicationSystem::getTickRate() const { return m_tick_rate; }

void ReplicationSystem::setInterpolationDelay(float seconds) { m_interpolation_delay = std::clamp(seconds, 0.0f, 0.5f); }

float ReplicationSystem::getInterpolationDelay() const { return m_interpolation_delay; }

void ReplicationSystem::setShowDebugUI(bool show) { m_show_debug_ui = show; }

std::vector<ReplicationLinkStats> ReplicationSystem::getLinkStats() const
{
    std::vector<ReplicationLinkStats> stats{};
    for (const auto& [peer, link] : m_links) {
        stats.push_back(link.stats);
    }
    std::sort(stats.begin(), stats.end(), [](const ReplicationLinkStats& a, const ReplicationLinkStats& b) { return a.peer < b.peer; });
    return stats;
}

size_t ReplicationSystem::getEntityCount() const { return m_entities.size(); }

uint32_t ReplicationSystem::computeStateDigest() const
{
    std::vector<const EntityRecord*> records{};
    for (const auto& [net_id, record] : m_entities) {
        records.push_back(&record);
    }
    std::sort(records.begin(), records.end(), [](const EntityRecord* a, const EntityRecord* b) { return a->net_id < b->net_id; });

    uint32_t hash = 2166136261u; // FNV-1a
    const auto mix = [&hash](std::span<const uint8_t> bytes) {
        for (const uint8_t byte : bytes) {
            hash ^= byte;
            hash *= 16777619u;
        }
    };

    std::array<uint8_t, MAX_COMPONENT_SIZE> buffer;
    for (const EntityRecord* const record : records) {
        const std::array<uint32_t, 3> header{record->net_id, record->archetype.getHash(), record->owner};
        std::array<uint8_t, sizeof(header)> header_bytes;
        std::memcpy(header_bytes.data(), header.data(), sizeof(header));
        mix(header_bytes);
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_types.size()); ++i) {
            const size_t size = serialiseComponent(*record, i, buffer);
            mix(std::span<const uint8_t>(buffer.data(), size));
        }
    }
    return hash;
}

void ReplicationSystem::addComponentType(const ComponentType& type)
{
    GC_ASSERT(m_entities.empty());
    if (m_component_types.size() >= MAX_COMPONENT_TYPES) {
        abortGame("Too many replicated component types (max {})", MAX_COMPONENT_TYPES);
    }
    m_component_types.push_back(type);
}

// Two hosts can only understand each other if they registered the same things in the same order
uint32_t ReplicationSystem::computeProtocolHash() const
{
    uint32_t hash = 2166136261u; // FNV-1a
    const auto mix = [&hash](uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            hash ^= (value >> (8 * i)) & 0xff;
            hash *= 16777619u;
        }
    };

    for (const ComponentType& type : m_component_types) {
        mix(type.name.getHash());
        mix(static_cast<uint32_t>(type.authority));
    }

    std::vector<std::pair<uint32_t, uint32_t>> archetypes{};
    for (const auto& [name, archetype] : m_archetypes) {
        archetypes.emplace_back(name.getHash(), static_cast<uint32_t>(archetype.transform_authority));
    }
    std::sort(archetypes.begin(), archetypes.end());
    for (const auto& [name_hash, transform_authority] : archetypes) {
        mix(name_hash);
        mix(transform_authority);
    }
    return hash;
}

void ReplicationSystem::updateRole()
{
    ReplicationRole new_role{};
    switch (m_net.getMode()) {
    case NetMode::SERVER:
        new_role = ReplicationRole::SERVER;
        break;
    case NetMode::CLIENT:
        new_role = ReplicationRole::CLIENT;
        break;
    default:
        new_role = ReplicationRole::OFFLINE;
        break;
    }
    if (new_role == m_role) {
        return;
    }

    GC_INFO("Replication role: {} -> {}", roleString(m_role), roleString(new_role));
    const ReplicationRole old_role = m_role;
    m_role = new_role;

    if (new_role == ReplicationRole::CLIENT) {
        becomeClient();
    }
    else if (old_role == ReplicationRole::CLIENT || old_role == ReplicationRole::NONE) {
        becomeAuthority();
    }
    else if (new_role == ReplicationRole::OFFLINE) {
        // The server was stopped. The world carries on without the clients.
        std::vector<NetPeerId> peers{};
        for (const auto& [peer, link] : m_links) {
            peers.push_back(peer);
        }
        for (const NetPeerId peer : peers) {
            onPeerLeft(peer);
        }
    }
    // Nothing to do when going from offline to server, clients will be told about the entities that already exist when they join
}

void ReplicationSystem::becomeAuthority()
{
    deleteAllEntities();
    m_links.clear();
    m_pending_despawns.clear();
    m_events.push_back(ReplicationEvent{.kind = ReplicationEventKind::AUTHORITY_STARTED});
}

// Everything that replicates now comes from the server
void ReplicationSystem::becomeClient()
{
    deleteAllEntities();
    m_links.clear();
    m_pending_despawns.clear();
    m_links[NET_PEER_SERVER].peer = NET_PEER_SERVER;
    m_links[NET_PEER_SERVER].stats.peer = NET_PEER_SERVER;
}

void ReplicationSystem::deleteAllEntities()
{
    for (const auto& [net_id, record] : m_entities) {
        if (isRecordAlive(record)) {
            m_world.deleteEntity(record.entity);
        }
    }
    m_entities.clear();
}

void ReplicationSystem::handleNetEvent(const NetEvent& ev)
{
    switch (ev.kind) {
    case NetEventKind::CONNECTED:
        if (m_role == ReplicationRole::SERVER) {
            onPeerJoined(ev.peer);
        }
        else if (m_role == ReplicationRole::CLIENT && ev.peer == NET_PEER_SERVER) {
            becomeClient(); // start from nothing, in case this is a reconnection
        }
        break;
    case NetEventKind::DISCONNECTED:
        if (hasAuthority()) {
            onPeerLeft(ev.peer);
        }
        break;
    case NetEventKind::MESSAGE:
        if (ev.type == MESSAGE_STATE) {
            onStateMessage(ev);
        }
        else if (m_role == ReplicationRole::CLIENT && ev.peer == NET_PEER_SERVER) {
            if (ev.type == MESSAGE_SPAWN) {
                onSpawnMessage(ev);
            }
            else if (ev.type == MESSAGE_DESPAWN) {
                onDespawnMessage(ev);
            }
            else if (ev.type == MESSAGE_WELCOME) {
                onWelcomeMessage(ev);
            }
        }
        break;
    }
}

void ReplicationSystem::onPeerJoined(NetPeerId peer)
{
    const auto [it, inserted] = m_links.try_emplace(peer);
    if (!inserted) {
        return;
    }
    it->second.peer = peer;
    it->second.stats.peer = peer;

    NetEvent welcome{};
    welcome.type = Name("repl_welcome");
    welcome.data.resize(sizeof(uint32_t));
    ByteWriter(welcome.data).writeU32(computeProtocolHash());
    m_net.postEvent(welcome, NetDelivery::RELIABLE, peer);

    // The entities that already exist are sent to the client at the next tick
    m_events.push_back(ReplicationEvent{.kind = ReplicationEventKind::PEER_JOINED, .peer = peer});
}

void ReplicationSystem::onPeerLeft(NetPeerId peer)
{
    const auto link_it = m_links.find(peer);
    if (link_it == m_links.end()) {
        return;
    }
    {
        const ReplicationLinkStats& stats = link_it->second.stats;
        GC_INFO("Replication link to peer {} closed: {} entities, {} states pending, state messages out/in {}/{}, states out/resent/in {}/{}/{}", peer,
                stats.entities, stats.pending, stats.messages_sent, stats.messages_received, stats.states_sent, stats.states_resent, stats.states_received);
    }
    m_links.erase(link_it);

    std::vector<NetEntityId> owned{};
    for (const auto& [net_id, record] : m_entities) {
        if (record.owner == peer) {
            owned.push_back(net_id);
        }
    }
    for (const NetEntityId net_id : owned) {
        destroyRecord(net_id, true);
        m_pending_despawns.push_back(net_id);
    }

    m_events.push_back(ReplicationEvent{.kind = ReplicationEventKind::PEER_LEFT, .peer = peer});
}

void ReplicationSystem::onWelcomeMessage(const NetEvent& ev)
{
    if (ev.data.size() != sizeof(uint32_t)) {
        return;
    }
    const uint32_t server_hash = ByteReader(ev.data).readU32();
    if (server_hash != computeProtocolHash()) {
        GC_ERROR("The server has different replicated components or archetypes to this client. Disconnecting.");
        m_net.disconnectFromServer();
    }
}

void ReplicationSystem::onSpawnMessage(const NetEvent& ev)
{
    constexpr size_t HEADER_SIZE = 4 * sizeof(uint32_t) + sizeof(uint8_t);
    if (ev.data.size() < HEADER_SIZE) {
        return;
    }
    ByteReader reader(ev.data);
    const NetEntityId net_id = reader.readU32();
    const Name archetype(reader.readU32());
    const NetPeerId owner = reader.readU32();
    // When the state was captured, not when it arrived: this message can be delayed for a long time by retransmissions, and
    // states captured after it (which can arrive just after it) must always count as newer.
    const double remote_time = static_cast<double>(reader.readU32()) * 1.0e-3;
    const uint32_t component_count = reader.readU8();

    if (net_id == NET_ENTITY_NONE || m_entities.contains(net_id)) {
        return;
    }
    if (!m_archetypes.contains(archetype)) {
        GC_ERROR("Server spawned an entity with unknown replication archetype {}", archetype);
    }

    Link& link = m_links[NET_PEER_SERVER];

    const Entity entity = m_world.createEntity(archetype);
    createRecord(entity, net_id, archetype, owner);
    link.items.try_emplace(net_id);

    // Entities on the client are always found through m_entities as the archetype can do anything, including spawning
    EntityRecord& record = m_entities.at(net_id);
    for (uint32_t i = 0; i < component_count; ++i) {
        if (reader.remaining() < COMPONENT_HEADER_SIZE) {
            break;
        }
        const uint32_t component_index = reader.readU8();
        const size_t size = reader.readU8();
        if (reader.remaining() < size || component_index >= m_component_types.size() || size > MAX_COMPONENT_SIZE) {
            break;
        }
        applyComponent(record, component_index, std::span<const uint8_t>(ev.data).subspan(reader.pos(), size), ApplyMode::INITIAL, remote_time);
        reader.skip(size);
    }
}

void ReplicationSystem::onDespawnMessage(const NetEvent& ev)
{
    if (ev.data.size() != sizeof(uint32_t)) {
        return;
    }
    const NetEntityId net_id = ByteReader(ev.data).readU32();
    destroyRecord(net_id, true);
    if (const auto link_it = m_links.find(NET_PEER_SERVER); link_it != m_links.end()) {
        link_it->second.items.erase(net_id);
    }
}

void ReplicationSystem::onStateMessage(const NetEvent& ev)
{
    const auto link_it = m_links.find(ev.peer);
    if (link_it == m_links.end() || ev.data.size() < STATE_HEADER_SIZE) {
        return;
    }
    Link& link = link_it->second;

    ByteReader reader(ev.data);
    const uint32_t seq = reader.readU32();
    const double remote_time = static_cast<double>(reader.readU32()) * 1.0e-3;
    const uint32_t ack_seq = reader.readU32();
    const uint32_t ack_bits = reader.readU32();

    if (seq == 0) {
        return;
    }
    const bool is_newest = seq > link.received_seq;
    if (!is_newest) {
        const uint32_t age = link.received_seq - seq;
        if (age >= 32 || ((link.received_bits >> age) & 1u)) {
            return; // too old, or a duplicate
        }
    }

    ++link.stats.messages_received;
    link.stats.bytes_received += ev.data.size();

    if (is_newest) {
        processAcks(link, ack_seq, ack_bits);

        if (m_role == ReplicationRole::CLIENT) {
            // Estimate what the server's clock says right now. Messages take a varying time to arrive so this is smoothed.
            // How much it varies, and how many messages go missing, decide how far in the past transforms have to be shown.
            constexpr double SMOOTHING = 0.05;
            const double offset = remote_time - m_time;
            if (!link.has_time_offset || std::abs(offset - link.time_offset) > 0.5) {
                link.time_offset = offset;
                link.has_time_offset = true;
            }
            else {
                link.jitter += (std::abs(offset - link.time_offset) - link.jitter) * SMOOTHING;
                link.time_offset += (offset - link.time_offset) * SMOOTHING;
            }
            const uint32_t missing = std::min(seq - link.received_seq - 1, 32u);
            for (uint32_t i = 0; i < missing; ++i) {
                link.loss += (1.0 - link.loss) * SMOOTHING;
            }
            link.loss += (0.0 - link.loss) * SMOOTHING;
        }
    }

    // If anything in the message can't be applied, the message isn't acknowledged so that the remote host sends its contents again.
    // That happens when state for an entity overtakes the (reliable, so possibly delayed) message that spawns the entity.
    // Everything else in the message is still applied.
    bool complete = true;
    bool malformed = false;
    while (reader.remaining() > 0 && !malformed) {
        if (reader.remaining() < ENTRY_HEADER_SIZE) {
            malformed = true;
            break;
        }
        const NetEntityId net_id = reader.readU32();
        const uint32_t component_count = reader.readU8();

        const auto record_it = m_entities.find(net_id);
        EntityRecord* const record = (record_it != m_entities.end()) ? &record_it->second : nullptr;
        if (!record) {
            complete = false; // its components are skipped over
        }

        for (uint32_t i = 0; i < component_count; ++i) {
            if (reader.remaining() < COMPONENT_HEADER_SIZE) {
                malformed = true;
                break;
            }
            const uint32_t component_index = reader.readU8();
            const size_t size = reader.readU8();
            if (reader.remaining() < size || component_index >= m_component_types.size() || size > MAX_COMPONENT_SIZE) {
                malformed = true;
                break;
            }
            // A host can only set the components it has authority over. Anything else is ignored.
            if (record && shouldAccept(*record, component_index, link.peer)) {
                const std::span<const uint8_t> data = std::span<const uint8_t>(ev.data).subspan(reader.pos(), size);
                if (seq > record->applied_seq[component_index]) {
                    applyComponent(*record, component_index, data, ApplyMode::UPDATE, remote_time);
                    record->applied_seq[component_index] = seq;
                    ++link.stats.states_received;
                }
                else if (component_index == TRANSFORM_INDEX) {
                    applyComponent(*record, component_index, data, ApplyMode::STALE, remote_time);
                }
            }
            reader.skip(size);
        }
    }

    complete = complete && !malformed;

    if (is_newest) {
        const uint32_t shift = seq - link.received_seq;
        link.received_bits = (shift >= 32) ? 0u : (link.received_bits << shift);
        link.received_seq = seq;
        if (complete) {
            link.received_bits |= 1u;
        }
    }
    else if (complete) {
        link.received_bits |= 1u << (link.received_seq - seq);
    }
    link.ack_pending = true;
}

ReplicationSystem::EntityRecord& ReplicationSystem::createRecord(Entity entity, NetEntityId net_id, Name archetype, NetPeerId owner)
{
    ReplicatedComponent& replicated = m_world.addComponent<ReplicatedComponent>(entity);
    replicated.net_id = net_id;
    replicated.archetype = archetype;
    replicated.owner = owner;

    ReplicationSpawnInfo info{};
    info.net_id = net_id;
    info.archetype = archetype;
    info.owner = owner;
    info.has_authority = hasAuthority();
    info.locally_owned = (owner != NET_PEER_NONE && owner == getLocalPeerId());

    {
        EntityRecord& record = m_entities[net_id];
        record.entity = entity;
        record.net_id = net_id;
        record.archetype = archetype;
        record.owner = owner;
        if (const auto it = m_archetypes.find(archetype); it != m_archetypes.end()) {
            record.transform_authority = it->second.transform_authority;
            record.interpolate_transform = it->second.interpolate_transform;
        }
    }

    // The callback is free to spawn more entities, which can move the records around, so nothing is held on to while it runs
    if (const auto it = m_archetypes.find(archetype); it != m_archetypes.end() && it->second.on_spawn) {
        it->second.on_spawn(m_world, entity, info);
    }

    return m_entities.at(net_id);
}

void ReplicationSystem::destroyRecord(NetEntityId net_id, bool delete_entity)
{
    const auto it = m_entities.find(net_id);
    if (it == m_entities.end()) {
        return;
    }
    if (delete_entity && isRecordAlive(it->second)) {
        m_world.deleteEntity(it->second.entity);
    }
    m_entities.erase(it);
}

// Entity handles are reused, so check that the entity is still the one the record was made for
bool ReplicationSystem::isRecordAlive(const EntityRecord& record) const
{
    const ReplicatedComponent* const replicated = m_world.getComponent<ReplicatedComponent>(record.entity);
    return replicated && replicated->net_id == record.net_id;
}

NetPeerId ReplicationSystem::getAuthorityPeer(const EntityRecord& record, uint32_t component_index) const
{
    const ReplicationAuthority authority = (component_index == TRANSFORM_INDEX) ? record.transform_authority : m_component_types[component_index].authority;
    if (authority == ReplicationAuthority::OWNER && record.owner != NET_PEER_NONE) {
        return record.owner;
    }
    return NET_PEER_SERVER;
}

bool ReplicationSystem::shouldSend(const EntityRecord& record, uint32_t component_index, NetPeerId to_peer) const
{
    const NetPeerId authority_peer = getAuthorityPeer(record, component_index);
    if (hasAuthority()) {
        // The server sends everything, including what it was told by other clients, but doesn't tell a client what that client decides
        return authority_peer != to_peer;
    }
    else {
        return authority_peer == getLocalPeerId();
    }
}

bool ReplicationSystem::shouldAccept(const EntityRecord& record, uint32_t component_index, NetPeerId from_peer) const
{
    const NetPeerId authority_peer = getAuthorityPeer(record, component_index);
    if (hasAuthority()) {
        return authority_peer == from_peer;
    }
    else {
        return authority_peer != getLocalPeerId();
    }
}

// returns the number of bytes written to out, 0 if the entity doesn't have the component
size_t ReplicationSystem::serialiseComponent(const EntityRecord& record, uint32_t component_index, std::span<uint8_t> out) const
{
    GC_ASSERT(out.size() >= MAX_COMPONENT_SIZE);

    if (component_index == TRANSFORM_INDEX) {
        const TransformComponent* const t = m_world.getComponent<TransformComponent>(record.entity);
        if (!t) {
            return 0;
        }
        const glm::vec3 position = t->getPosition();
        const glm::quat rotation = t->getRotation();
        const glm::vec3 scale = t->getScale();
        const bool has_scale = (scale != glm::vec3{1.0f, 1.0f, 1.0f});

        ByteWriter writer(out);
        writer.writeU8(has_scale ? TRANSFORM_FLAG_HAS_SCALE : uint8_t{0});
        writer.writeF32(position.x);
        writer.writeF32(position.y);
        writer.writeF32(position.z);
        writer.writeF32(rotation.w);
        writer.writeF32(rotation.x);
        writer.writeF32(rotation.y);
        writer.writeF32(rotation.z);
        if (has_scale) {
            writer.writeF32(scale.x);
            writer.writeF32(scale.y);
            writer.writeF32(scale.z);
        }
        return writer.pos();
    }

    // Written to a larger buffer first so that a component that writes too much is caught instead of overflowing
    std::array<uint8_t, MAX_COMPONENT_SIZE * 4> scratch;
    ByteWriter writer(scratch);
    if (!m_component_types[component_index].serialise(m_world, record.entity, writer)) {
        return 0;
    }
    if (writer.pos() > MAX_COMPONENT_SIZE) {
        GC_ERROR_ONCE("Replicated component {} serialises to {} bytes (max {})", m_component_types[component_index].name, writer.pos(), MAX_COMPONENT_SIZE);
        return 0;
    }
    std::memcpy(out.data(), scratch.data(), writer.pos());
    return writer.pos();
}

void ReplicationSystem::applyComponent(EntityRecord& record, uint32_t component_index, std::span<const uint8_t> data, ApplyMode mode, double remote_time)
{
    if (!isRecordAlive(record)) {
        return;
    }
    const bool interpolated = (m_role == ReplicationRole::CLIENT && record.interpolate_transform);

    if (component_index == TRANSFORM_INDEX) {
        if (data.size() != TRANSFORM_SIZE && data.size() != TRANSFORM_SIZE_WITH_SCALE) {
            return;
        }
        ByteReader reader(data);
        const uint8_t flags = reader.readU8();
        const bool has_scale = (flags & TRANSFORM_FLAG_HAS_SCALE) != 0;
        if (has_scale != (data.size() == TRANSFORM_SIZE_WITH_SCALE)) {
            return;
        }
        TransformSample sample{};
        sample.time = remote_time;
        sample.position.x = reader.readF32();
        sample.position.y = reader.readF32();
        sample.position.z = reader.readF32();
        sample.rotation.w = reader.readF32();
        sample.rotation.x = reader.readF32();
        sample.rotation.y = reader.readF32();
        sample.rotation.z = reader.readF32();
        glm::vec3 scale{1.0f, 1.0f, 1.0f};
        if (has_scale) {
            scale.x = reader.readF32();
            scale.y = reader.readF32();
            scale.z = reader.readF32();
        }

        // This came from the network, so make sure it can't break anything
        const float rotation_length = glm::length(sample.rotation);
        if (!isFinite(sample.position) || !isFinite(scale) || !std::isfinite(rotation_length) || rotation_length < 1.0e-3f) {
            return;
        }
        // Rotations that are already (near enough) normalised are left exactly as they were sent
        if (std::abs(rotation_length - 1.0f) > 1.0e-3f) {
            sample.rotation /= rotation_length;
        }

        if (mode == ApplyMode::STALE) {
            // A state that was overtaken by a newer one is still somewhere the entity was
            if (interpolated) {
                pushTransformSample(record, sample, false);
            }
            return;
        }

        TransformComponent* const t = m_world.getComponent<TransformComponent>(record.entity);
        GC_ASSERT(t);
        if (t->getScale() != scale) {
            t->setScale(scale);
        }
        if (mode == ApplyMode::INITIAL) {
            record.sample_count = 0;
        }
        if (interpolated) {
            pushTransformSample(record, sample, true);
        }
        if (mode == ApplyMode::INITIAL || !interpolated) {
            t->setPosition(sample.position);
            t->setRotation(sample.rotation);
        }
        return;
    }

    // Zero padded so that a component that reads more than it was sent can't read out of bounds
    std::array<uint8_t, MAX_COMPONENT_SIZE * 4> scratch{};
    std::memcpy(scratch.data(), data.data(), data.size());
    ByteReader reader(scratch);
    m_component_types[component_index].deserialise(m_world, record.entity, reader);
}

// Samples are kept in time order.
// is_latest: this is the most recent state of the entity, so it must end up as the newest sample. That's what the entity is left
// showing if nothing else arrives, and every host has to end up showing the same thing.
void ReplicationSystem::pushTransformSample(EntityRecord& record, TransformSample sample, bool is_latest)
{
    if (is_latest && record.sample_count > 0 && sample.time <= record.samples[record.sample_count - 1].time) {
        // Shouldn't happen, as newer states have later times. But if it does, being correct matters more than being smooth.
        sample.time = record.samples[record.sample_count - 1].time;
    }

    // State is only sent when it changes. If nothing was received for a long time then the entity was stationary until just
    // before this sample, so say so. Otherwise it would be shown drifting towards the new state for the whole gap.
    // Shorter gaps are more likely to be lost messages, which interpolation is supposed to hide.
    constexpr double HOLD_TIME = 0.5;
    if (record.sample_count > 0 && sample.time - record.samples[record.sample_count - 1].time > HOLD_TIME) {
        TransformSample hold = record.samples[record.sample_count - 1];
        hold.time = sample.time - 1.0 / static_cast<double>(m_tick_rate);
        record.sample_count = 0;
        record.samples[record.sample_count++] = hold;
    }

    // Usually the sample is the newest, but state messages can arrive out of order
    uint32_t index = record.sample_count;
    while (index > 0 && record.samples[index - 1].time >= sample.time) {
        --index;
    }
    if (index < record.sample_count && record.samples[index].time == sample.time) {
        record.samples[index] = sample;
        return;
    }
    if (record.sample_count == record.samples.size()) {
        if (index == 0) {
            return; // older than everything, and there is no room for it
        }
        // drop the oldest
        std::move(record.samples.begin() + 1, record.samples.begin() + record.sample_count, record.samples.begin());
        --record.sample_count;
        --index;
    }
    std::move_backward(record.samples.begin() + index, record.samples.begin() + record.sample_count, record.samples.begin() + record.sample_count + 1);
    record.samples[index] = sample;
    ++record.sample_count;
}

// Shows entities controlled by other hosts where they were a short time ago, which means there are (almost) always
// states either side of that time to blend between.
void ReplicationSystem::interpolateTransforms()
{
    const auto link_it = m_links.find(NET_PEER_SERVER);
    if (link_it == m_links.end() || !link_it->second.has_time_offset) {
        return;
    }
    Link& link = link_it->second;

    // The delay has to be long enough that a newer state has (nearly) always arrived by the time it's needed.
    // States are a tick apart, more when messages are lost, and arrive early or late by the jitter.
    {
        const double tick_interval = 1.0 / static_cast<double>(m_tick_rate);
        // (Generous, as an entity that keeps stopping and starting looks far worse than one that is a little further behind)
        const double wanted = tick_interval * (2.5 + std::min(link.loss * 15.0, 4.0)) + link.jitter * 4.0;
        // The limit keeps the delay within the history of states that is kept for each entity (EntityRecord::samples)
        constexpr double MAX_DELAY = 0.5;
        const double target = std::clamp(wanted, static_cast<double>(m_interpolation_delay), MAX_DELAY);
        if (link.interpolation_delay == 0.0) {
            link.interpolation_delay = target;
        }
        // Changing the delay speeds up or slows down everything that is interpolated, so do it gently.
        // Being too short is the worse problem (entities stop and start) so that is corrected faster.
        link.interpolation_delay += std::clamp(target - link.interpolation_delay, -m_delta_time * 0.05, m_delta_time * 0.25);

        link.stats.jitter = static_cast<float>(link.jitter);
        link.stats.loss = static_cast<float>(link.loss);
        link.stats.interpolation_delay = static_cast<float>(link.interpolation_delay);
    }

    const double render_time = m_time + link.time_offset - link.interpolation_delay;
    const NetPeerId local_peer = getLocalPeerId();

    for (auto& [net_id, record] : m_entities) {
        if (!record.interpolate_transform || record.sample_count == 0 || getAuthorityPeer(record, TRANSFORM_INDEX) == local_peer) {
            continue;
        }
        TransformComponent* const t = m_world.getComponent<TransformComponent>(record.entity);
        if (!t || !isRecordAlive(record)) {
            continue;
        }

        glm::vec3 position{};
        glm::quat rotation{};
        const TransformSample& oldest = record.samples[0];
        const TransformSample& newest = record.samples[record.sample_count - 1];
        if (render_time <= oldest.time) {
            position = oldest.position;
            rotation = oldest.rotation;
        }
        else if (render_time >= newest.time) {
            // Nothing newer has arrived. Hold the last known state instead of guessing.
            position = newest.position;
            rotation = newest.rotation;
        }
        else {
            uint32_t i = 1;
            while (record.samples[i].time < render_time) {
                ++i;
            }
            const TransformSample& a = record.samples[i - 1];
            const TransformSample& b = record.samples[i];
            if (glm::distance(a.position, b.position) > m_teleport_distance) {
                position = a.position; // a teleport, not movement
                rotation = a.rotation;
            }
            else {
                const float alpha = static_cast<float>((render_time - a.time) / (b.time - a.time));
                position = glm::mix(a.position, b.position, alpha);
                rotation = glm::slerp(a.rotation, b.rotation, alpha);
            }
        }

        // setting the transform has a cost (the world matrix is recalculated) so only do it when needed
        if (t->getPosition() != position) {
            t->setPosition(position);
        }
        if (t->getRotation() != rotation) {
            t->setRotation(rotation);
        }
    }
}

void ReplicationSystem::tick()
{
    ZoneScoped;

    ++m_tick_count;

    removeDeletedEntities();

    if (m_links.empty()) {
        m_pending_despawns.clear();
        return; // nobody to send anything to
    }

    captureStates();

    if (hasAuthority()) {
        for (auto& [peer, link] : m_links) {
            sendSpawns(link);
        }
        sendDespawns();
    }

    for (auto& [peer, link] : m_links) {
        sendStates(link);
    }
}

// Finds replicated entities that were deleted without telling this system
void ReplicationSystem::removeDeletedEntities()
{
    for (auto it = m_entities.begin(); it != m_entities.end();) {
        if (isRecordAlive(it->second)) {
            ++it;
        }
        else {
            if (hasAuthority()) {
                m_pending_despawns.push_back(it->first);
            }
            it = m_entities.erase(it);
        }
    }
}

// Serialises every component this host might need to send, to find out which ones have changed
void ReplicationSystem::captureStates()
{
    const bool authority = hasAuthority();
    const NetPeerId local_peer = getLocalPeerId();
    std::array<uint8_t, MAX_COMPONENT_SIZE> buffer;

    for (auto& [net_id, record] : m_entities) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_types.size()); ++i) {
            // A client only ever sends what it controls. The server sends everything to someone.
            if (!authority && getAuthorityPeer(record, i) != local_peer) {
                continue;
            }
            const size_t size = serialiseComponent(record, i, buffer);
            if (size == 0) {
                continue;
            }
            ComponentState& state = record.states[i];
            if (state.version == 0 || state.size != size || std::memcmp(state.bytes.data(), buffer.data(), size) != 0) {
                std::memcpy(state.bytes.data(), buffer.data(), size);
                state.size = static_cast<uint8_t>(size);
                ++state.version;
            }
        }
    }
}

// Tells the remote host about every entity it doesn't know about yet. That's all of them if it has just joined.
void ReplicationSystem::sendSpawns(Link& link)
{
    for (const auto& [net_id, record] : m_entities) {
        if (link.items.contains(net_id)) {
            continue;
        }
        auto& items = link.items[net_id];

        NetEvent ev{};
        ev.type = Name("repl_spawn");
        ev.data.resize(4 * sizeof(uint32_t) + sizeof(uint8_t) + m_component_types.size() * (COMPONENT_HEADER_SIZE + MAX_COMPONENT_SIZE));
        ByteWriter writer(ev.data);
        writer.writeU32(net_id);
        writer.writeU32(record.archetype.getHash());
        writer.writeU32(record.owner);
        writer.writeU32(static_cast<uint32_t>(m_time * 1.0e3));
        const size_t count_pos = writer.pos();
        writer.writeU8(0);

        uint8_t component_count = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_types.size()); ++i) {
            const ComponentState& state = record.states[i];
            if (state.version == 0) {
                continue;
            }
            writer.writeU8(static_cast<uint8_t>(i));
            writer.writeU8(state.size);
            writer.writeBytes(std::span<const uint8_t>(state.bytes.data(), state.size));
            ++component_count;

            // Reliable, so the remote host is going to get this version
            items[i].sent_version = state.version;
            items[i].acked_version = state.version;
        }
        ev.data[count_pos] = component_count;
        ev.data.resize(writer.pos());

        m_net.postEvent(ev, NetDelivery::RELIABLE, link.peer);
    }
}

void ReplicationSystem::sendDespawns()
{
    for (const NetEntityId net_id : m_pending_despawns) {
        NetEvent ev{};
        ev.type = Name("repl_despawn");
        ev.data.resize(sizeof(uint32_t));
        ByteWriter(ev.data).writeU32(net_id);
        for (auto& [peer, link] : m_links) {
            // hosts that were never told about the entity don't need to know it's gone
            if (link.items.erase(net_id) > 0) {
                m_net.postEvent(ev, NetDelivery::RELIABLE, peer);
            }
        }
    }
    m_pending_despawns.clear();
}

void ReplicationSystem::processAcks(Link& link, uint32_t ack_seq, uint32_t ack_bits)
{
    for (auto& [net_id, items] : link.items) {
        for (ItemState& item : items) {
            if (!item.in_flight || item.sent_seq > ack_seq) {
                continue;
            }
            const uint32_t age = ack_seq - item.sent_seq;
            if (age < 32 && ((ack_bits >> age) & 1u)) {
                item.acked_version = item.sent_version;
            }
            // otherwise it was lost, and is sent again at the next tick
            item.in_flight = false;
        }
    }
}

// Sends the components that have changed since the remote host last acknowledged them
void ReplicationSystem::sendStates(Link& link)
{
    struct Candidate {
        NetEntityId net_id;
        uint64_t age; // ticks since anything was sent for this entity
    };
    std::vector<Candidate> candidates{};

    const auto needsSending = [&](const EntityRecord& record, const ItemState& item, uint32_t component_index) {
        const ComponentState& state = record.states[component_index];
        if (state.version == 0 || item.acked_version == state.version || !shouldSend(record, component_index, link.peer)) {
            return false;
        }
        // don't send the same thing again while the last attempt might still be on its way
        const bool waiting = item.in_flight && item.sent_version == state.version && (m_tick_count - item.sent_tick) < RESEND_TICKS;
        return !waiting;
    };

    uint32_t pending = 0;
    for (auto& [net_id, items] : link.items) {
        const auto record_it = m_entities.find(net_id);
        if (record_it == m_entities.end()) {
            continue;
        }
        const EntityRecord& record = record_it->second;
        bool needed = false;
        uint64_t age = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_types.size()); ++i) {
            const ComponentState& state = record.states[i];
            if (state.version != 0 && items[i].acked_version != state.version && shouldSend(record, i, link.peer)) {
                ++pending;
            }
            if (needsSending(record, items[i], i)) {
                needed = true;
                age = std::max(age, m_tick_count - items[i].sent_tick);
            }
        }
        if (needed) {
            candidates.push_back(Candidate{net_id, age});
        }
    }
    link.stats.entities = static_cast<uint32_t>(link.items.size());
    link.stats.pending = pending;

    if (candidates.empty() && !link.ack_pending) {
        return;
    }

    // If there is more than can be sent this tick, the entities that have waited the longest go first
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.age > b.age; });

    size_t next_candidate = 0;
    for (int message = 0; message < MAX_STATE_MESSAGES_PER_TICK; ++message) {
        NetEvent ev{};
        ev.type = Name("repl_state");
        ev.data.resize(STATE_MESSAGE_BUDGET);
        ByteWriter writer(ev.data);
        const uint32_t seq = link.next_seq++;
        writer.writeU32(seq);
        writer.writeU32(static_cast<uint32_t>(m_time * 1.0e3));
        writer.writeU32(link.received_seq);
        writer.writeU32(link.received_bits);

        for (; next_candidate < candidates.size(); ++next_candidate) {
            const NetEntityId net_id = candidates[next_candidate].net_id;
            const EntityRecord& record = m_entities.at(net_id);
            auto& items = link.items.at(net_id);

            if (writer.remaining() < ENTRY_HEADER_SIZE) {
                break;
            }
            const size_t entry_pos = writer.pos();
            writer.writeU32(net_id);
            writer.writeU8(0);

            uint8_t component_count = 0;
            bool all_written = true;
            for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_types.size()); ++i) {
                if (!needsSending(record, items[i], i)) {
                    continue;
                }
                const ComponentState& state = record.states[i];
                if (writer.remaining() < COMPONENT_HEADER_SIZE + state.size) {
                    all_written = false;
                    continue; // a smaller component might still fit
                }
                writer.writeU8(static_cast<uint8_t>(i));
                writer.writeU8(state.size);
                writer.writeBytes(std::span<const uint8_t>(state.bytes.data(), state.size));
                ++component_count;

                ItemState& item = items[i];
                if (item.sent_version == state.version) {
                    ++link.stats.states_resent;
                }
                item.sent_version = state.version;
                item.sent_seq = seq;
                item.sent_tick = m_tick_count;
                item.in_flight = true;
                ++link.stats.states_sent;
            }

            if (component_count == 0) {
                // nothing fitted, so take the entity back out of the message
                writer.reset();
                writer.skip(entry_pos);
            }
            else {
                ev.data[entry_pos + sizeof(uint32_t)] = component_count;
            }
            if (!all_written) {
                break; // the rest of this entity goes in the next message
            }
        }

        ev.data.resize(writer.pos());
        m_net.postEvent(ev, NetDelivery::UNRELIABLE, link.peer);
        link.ack_pending = false;
        ++link.stats.messages_sent;
        link.stats.bytes_sent += ev.data.size();

        if (next_candidate >= candidates.size()) {
            break;
        }
    }
}

void ReplicationSystem::renderDebugUI(bool* open)
{
    ImGui::SetNextWindowPos(ImVec2(530.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Replication", open)) {
        ImGui::Text("Role: %s", roleString(m_role));
        ImGui::Text("Local peer ID: %u", getLocalPeerId());
        ImGui::Text("Replicated entities: %u", static_cast<unsigned>(m_entities.size()));

        ImGui::SliderFloat("Tick rate", &m_tick_rate, 1.0f, 120.0f, "%.0f Hz", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Min interp. delay", &m_interpolation_delay, 0.0f, 0.5f, "%.3f s", ImGuiSliderFlags_AlwaysClamp);
        if (m_role == ReplicationRole::CLIENT) {
            for (const ReplicationLinkStats& stats : getLinkStats()) {
                ImGui::Text("Interpolation delay: %.0f ms (state messages: jitter %.1f ms, loss %.1f %%)", stats.interpolation_delay * 1.0e3f,
                            stats.jitter * 1.0e3f, stats.loss * 100.0f);
            }
        }

        // columns are as wide as their contents, with a scrollbar if the window is too narrow for them
        constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX;

        if (ImGui::CollapsingHeader("Links", ImGuiTreeNodeFlags_DefaultOpen)) {
            const ImVec2 links_size(0.0f, ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(m_links.size()) + 2.5f));
            if (ImGui::BeginTable("repl_links", 6, TABLE_FLAGS, links_size)) {
                ImGui::TableSetupColumn("Peer");
                ImGui::TableSetupColumn("Entities");
                ImGui::TableSetupColumn("Pending");
                ImGui::TableSetupColumn("Msgs out/in");
                ImGui::TableSetupColumn("States out/resent/in");
                ImGui::TableSetupColumn("Bytes out/in");
                ImGui::TableHeadersRow();
                for (const ReplicationLinkStats& stats : getLinkStats()) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", stats.peer);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", stats.entities);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", stats.pending);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::format("{} / {}", stats.messages_sent, stats.messages_received).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::format("{} / {} / {}", stats.states_sent, stats.states_resent, stats.states_received).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::format("{} / {}", stats.bytes_sent, stats.bytes_received).c_str());
                }
                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader("Entities")) {
            std::vector<const EntityRecord*> records{};
            for (const auto& [net_id, record] : m_entities) {
                records.push_back(&record);
            }
            std::sort(records.begin(), records.end(), [](const EntityRecord* a, const EntityRecord* b) { return a->net_id < b->net_id; });

            if (ImGui::BeginTable("repl_entities", 5, TABLE_FLAGS | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f))) {
                ImGui::TableSetupColumn("Net ID");
                ImGui::TableSetupColumn("Archetype");
                ImGui::TableSetupColumn("Owner");
                ImGui::TableSetupColumn("Transform set by");
                ImGui::TableSetupColumn("Entity");
                ImGui::TableHeadersRow();
                for (const EntityRecord* const record : records) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", record->net_id);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(record->archetype.getString().c_str());
                    ImGui::TableNextColumn();
                    if (record->owner == NET_PEER_NONE) {
                        ImGui::TextUnformatted("-");
                    }
                    else {
                        ImGui::Text("%u", record->owner);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", getAuthorityPeer(*record, TRANSFORM_INDEX));
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", record->entity);
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}

} // namespace gc
