#include "hud.h"

#include <algorithm>
#include <vector>

#include <imgui.h>

#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_replication.h>
#include <gamecore/gc_world.h>

#include "arena.h"

HudSystem::HudSystem(gc::World& world, gc::ReplicationSystem& replication) : gc::System(world), m_replication(replication) {}

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
    m_world.forEach<gc::ReplicatedComponent, PlayerComponent>([&](gc::Entity, const gc::ReplicatedComponent& replicated, const PlayerComponent& player) {
        rows.push_back(Row{replicated.owner, player});
    });
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.player.score > b.player.score; });

    const gc::NetPeerId local_peer = m_replication.getLocalPeerId();

    constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    // the work area starts below the debug menu bar, when that is shown (F10)
    const ImVec2 work_pos = ImGui::GetMainViewport()->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(work_pos.x + 10.0f, work_pos.y + 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("Scoreboard", nullptr, FLAGS)) {
        if (rows.empty()) {
            ImGui::TextUnformatted("Waiting for the server...");
        }
        else if (ImGui::BeginTable("scoreboard", 4, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Player");
            ImGui::TableSetupColumn("Score");
            ImGui::TableSetupColumn("Health");
            ImGui::TableSetupColumn("Deaths");
            ImGui::TableHeadersRow();
            for (const Row& row : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text(row.peer == local_peer ? "%u (you)" : "%u", row.peer);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.player.score);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.player.health);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.player.deaths);
            }
            ImGui::EndTable();
        }
        ImGui::TextUnformatted("WASD/Space/Shift: move   Mouse: look and fire   F10: debug windows");
    }
    ImGui::End();

    // crosshair
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::GetForegroundDrawList()->AddCircleFilled(centre, 3.0f, IM_COL32(255, 255, 255, 200));
}
