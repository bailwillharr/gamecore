#include "hud.h"

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_replication.h>
#include <gamecore/gc_world.h>

#include "components.h"
#include "match.h"

static constexpr ImGuiWindowFlags OVERLAY_FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                                  ImGuiWindowFlags_NoInputs;

HudSystem::HudSystem(gc::World& world, gc::ReplicationSystem& replication, MatchSystem& match) : gc::System(world), m_replication(replication), m_match(match)
{
}

void HudSystem::onUpdate([[maybe_unused]] gc::FrameState& frame_state)
{
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    struct Row {
        gc::NetPeerId peer;
        PlayerComponent player;
    };
    std::vector<Row> rows{};
    m_world.forEach<gc::ReplicatedComponent, PlayerComponent>(
        [&](gc::Entity, const gc::ReplicatedComponent& replicated, const PlayerComponent& player) { rows.push_back(Row{replicated.owner, player}); });
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.player.score > b.player.score; });

    const gc::NetPeerId local_peer = m_replication.getLocalPeerId();
    const auto getPlayerName = [local_peer](gc::NetPeerId peer) { return (peer == local_peer) ? std::string("You") : "Player " + std::to_string(peer); };

    // the work area starts below the debug menu bar, when that is shown (F10)
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    const ImVec2 work_pos = viewport->WorkPos;
    const ImVec2 work_size = viewport->WorkSize;

    // scoreboard, top left
    ImGui::SetNextWindowPos(ImVec2(work_pos.x + 10.0f, work_pos.y + 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("Scoreboard", nullptr, OVERLAY_FLAGS)) {
        if (rows.empty()) {
            ImGui::TextUnformatted("Waiting for the server...");
        }
        else if (ImGui::BeginTable("scoreboard", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Player");
            ImGui::TableSetupColumn("Score");
            ImGui::TableSetupColumn("Deaths");
            ImGui::TableHeadersRow();
            for (const Row& row : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(getPlayerName(row.peer).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.player.score);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.player.deaths);
            }
            ImGui::EndTable();
        }
        ImGui::TextUnformatted("WASD: move   Space: jump   Mouse: look and throw   F10: debug windows");
    }
    ImGui::End();

    // who burned who, top right. Each line fades away after a few seconds
    constexpr double KILL_FEED_TIME = 6.0;
    const double time = m_match.getTime();
    const auto& kill_feed = m_match.getKillFeed();
    if (std::any_of(kill_feed.begin(), kill_feed.end(), [&](const auto& entry) { return time - entry.time < KILL_FEED_TIME; })) {
        ImGui::SetNextWindowPos(ImVec2(work_pos.x + work_size.x - 10.0f, work_pos.y + 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.5f);
        if (ImGui::Begin("Kill feed", nullptr, OVERLAY_FLAGS)) {
            for (const MatchSystem::KillFeedEntry& entry : kill_feed) {
                if (time - entry.time < KILL_FEED_TIME) {
                    ImGui::Text("%s burned %s", getPlayerName(entry.killer).c_str(), getPlayerName(entry.victim).c_str());
                }
            }
        }
        ImGui::End();
    }

    // the local player's health, bottom left
    const auto local_row = std::find_if(rows.begin(), rows.end(), [local_peer](const Row& row) { return row.peer == local_peer; });
    if (local_row != rows.end()) {
        ImGui::SetNextWindowPos(ImVec2(work_pos.x + 10.0f, work_pos.y + work_size.y - 10.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.5f);
        if (ImGui::Begin("Health", nullptr, OVERLAY_FLAGS)) {
            const float fraction = std::clamp(static_cast<float>(local_row->player.health) / static_cast<float>(PLAYER_MAX_HEALTH), 0.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f - fraction, fraction * 0.8f, 0.1f, 1.0f));
            ImGui::ProgressBar(fraction, ImVec2(220.0f, 0.0f), std::to_string(local_row->player.health).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("Embers: %d", local_row->player.score);
        }
        ImGui::End();

        // The crosshair. It turns into a red cross for a moment when the server says that a bolt hit something.
        constexpr double HIT_MARKER_TIME = 0.25;
        const ImVec2 centre = viewport->GetCenter();
        ImDrawList* const draw_list = ImGui::GetForegroundDrawList();
        if (m_match.getLastHitTime() >= 0.0 && time - m_match.getLastHitTime() < HIT_MARKER_TIME) {
            constexpr float SIZE = 9.0f;
            draw_list->AddLine(ImVec2(centre.x - SIZE, centre.y - SIZE), ImVec2(centre.x + SIZE, centre.y + SIZE), IM_COL32(255, 60, 40, 255), 2.5f);
            draw_list->AddLine(ImVec2(centre.x - SIZE, centre.y + SIZE), ImVec2(centre.x + SIZE, centre.y - SIZE), IM_COL32(255, 60, 40, 255), 2.5f);
        }
        else {
            draw_list->AddCircleFilled(centre, 3.0f, IM_COL32(255, 255, 255, 200));
        }
    }
}
