#include "hud.h"

#include <algorithm>
#include <string>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_replication.h>
#include <gamecore/gc_ui.h>
#include <gamecore/gc_ui_component.h>
#include <gamecore/gc_world.h>

#include "components.h"
#include "match.h"

using namespace gc::literals;

static constexpr const char* MODEL_NAME = "ember_hud";

// Sets a bound variable, and tells the data model if it changed (so that only what changed is laid out again)
template <typename T>
static void setVariable(Rml::DataModelHandle& model, const char* name, T& variable, const T& value)
{
    if (!(variable == value)) {
        variable = value;
        model.DirtyVariable(name);
    }
}

HudSystem::HudSystem(gc::World& world, gc::UI& ui, gc::ReplicationSystem& replication, MatchSystem& match)
    : gc::System(world), m_ui(ui), m_replication(replication), m_match(match)
{
    // The data model has to exist before the document is loaded (by the UISystem, which is registered after this)
    Rml::DataModelConstructor model = m_ui.getContext().CreateDataModel(MODEL_NAME);
    if (!model) {
        gc::abortGame("Failed to create the HUD's data model");
    }
    if (auto row = model.RegisterStruct<ScoreRow>()) {
        row.RegisterMember("name", &ScoreRow::name);
        row.RegisterMember("score", &ScoreRow::score);
        row.RegisterMember("deaths", &ScoreRow::deaths);
        row.RegisterMember("local", &ScoreRow::local);
    }
    model.RegisterArray<std::vector<ScoreRow>>();
    if (auto kill = model.RegisterStruct<KillRow>()) {
        kill.RegisterMember("killer", &KillRow::killer);
        kill.RegisterMember("victim", &KillRow::victim);
        kill.RegisterMember("opacity", &KillRow::opacity);
    }
    model.RegisterArray<std::vector<KillRow>>();
    model.Bind("waiting", &m_waiting);
    model.Bind("rows", &m_rows);
    model.Bind("kills", &m_kills);
    model.Bind("alive", &m_alive);
    model.Bind("health", &m_health);
    model.Bind("health_fraction", &m_health_fraction);
    model.Bind("score", &m_score);
    model.Bind("hit", &m_hit);
    m_model = model.GetModelHandle();

    m_hud = m_world.createEntity("hud"_name);
    m_world.addComponent<gc::UIComponent>(m_hud).document = "ember_court_hud.rml"_name;
}

HudSystem::~HudSystem()
{
    // (the document stays open until the UISystem goes, which is after this. It is detached from the model)
    m_ui.getContext().RemoveDataModel(MODEL_NAME);
}

void HudSystem::onUpdate([[maybe_unused]] gc::FrameState& frame_state)
{
    struct Player {
        gc::NetPeerId peer;
        PlayerComponent player;
    };
    std::vector<Player> players{};
    m_world.forEach<gc::ReplicatedComponent, PlayerComponent>(
        [&](gc::Entity, const gc::ReplicatedComponent& replicated, const PlayerComponent& player) { players.push_back(Player{replicated.owner, player}); });
    std::sort(players.begin(), players.end(), [](const Player& a, const Player& b) { return a.player.score > b.player.score; });

    const gc::NetPeerId local_peer = m_replication.getLocalPeerId();
    const auto getPlayerName = [local_peer](gc::NetPeerId peer) { return (peer == local_peer) ? std::string("You") : "Player " + std::to_string(peer); };

    // the scoreboard
    std::vector<ScoreRow> rows{};
    for (const Player& p : players) {
        rows.push_back(ScoreRow{getPlayerName(p.peer), p.player.score, p.player.deaths, p.peer == local_peer});
    }
    setVariable(m_model, "waiting", m_waiting, players.empty());
    setVariable(m_model, "rows", m_rows, rows);

    // who burned who. Each line fades away after a few seconds
    constexpr double KILL_FEED_TIME = 6.0;
    constexpr double KILL_FEED_FADE_TIME = 1.0;
    const double time = m_match.getTime();
    std::vector<KillRow> kills{};
    for (const MatchSystem::KillFeedEntry& entry : m_match.getKillFeed()) {
        const double age = time - entry.time;
        if (age < KILL_FEED_TIME) {
            const float opacity = static_cast<float>(std::clamp((KILL_FEED_TIME - age) / KILL_FEED_FADE_TIME, 0.0, 1.0));
            kills.push_back(KillRow{getPlayerName(entry.killer), getPlayerName(entry.victim), opacity});
        }
    }
    setVariable(m_model, "kills", m_kills, kills);

    // the local player's health, and the crosshair. It turns into a red cross for a moment when the server says that a bolt hit something.
    const auto local = std::find_if(players.begin(), players.end(), [local_peer](const Player& p) { return p.peer == local_peer; });
    setVariable(m_model, "alive", m_alive, local != players.end());
    if (local != players.end()) {
        setVariable(m_model, "health", m_health, static_cast<int>(local->player.health));
        setVariable(m_model, "health_fraction", m_health_fraction,
                    std::clamp(static_cast<float>(local->player.health) / static_cast<float>(PLAYER_MAX_HEALTH), 0.0f, 1.0f));
        setVariable(m_model, "score", m_score, static_cast<int>(local->player.score));
    }
    constexpr double HIT_MARKER_TIME = 0.25;
    setVariable(m_model, "hit", m_hit, m_match.getLastHitTime() >= 0.0 && time - m_match.getLastHitTime() < HIT_MARKER_TIME);}
