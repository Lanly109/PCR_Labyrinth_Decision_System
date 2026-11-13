#include "labyrinth.hpp"

namespace labyrinth {
Json check_score(const Request& request, const Config& config, const Catalog& catalog) {
    Json reply = {{"request_id", request.request_id}, {"action", "noop"}};
    const auto& body = request.body;
    const auto guild = config.guild_minimum_scores.find(body.at("guild_id").get<Id>());
    const int minimum = guild == config.guild_minimum_scores.end() ? 0 : guild->second;
    const auto quest = catalog.quests.find(body.at("quest_id").get<Id>());
    const auto rate = catalog.score_rates.find(request.difficulty);
    const auto final_region = request.difficulty == 1 ? 3 : 5;
    if (quest == catalog.quests.end() || quest->second.type != 3 ||
        rate == catalog.score_rates.end() || request.region != final_region) return reply;

    int64_t base = 0;
    for (const auto& area : body.at("areas")) {
        const auto region = area.at("region").get<int>();
        const auto cleared = area.at("cleared").get<bool>();
        if (cleared != (region < final_region)) return reply;
        const auto move = catalog.score_settings.find(210 + region);
        const auto completion = catalog.score_settings.find(200 + region);
        if (move == catalog.score_settings.end() || completion == catalog.score_settings.end()) return reply;
        auto blocks = area.at("block_count").get<int64_t>();
        if (!cleared) ++blocks;  // The pending final Boss also clears its tile and area.
        base += blocks * move->second + completion->second;
    }

    const std::array<std::string_view, 5> counts{
        "unit_count", "relic_count", "normal_clear_count", "ex_clear_count", "boss_clear_count"
    };
    for (size_t i = 0; i < counts.size(); ++i) {
        const auto points = catalog.score_settings.find(221 + static_cast<int>(i));
        if (points == catalog.score_settings.end()) return reply;
        auto count = body.at(counts[i]).get<int64_t>();
        if (counts[i] == "boss_clear_count") ++count;  // Project the pending final Boss victory.
        base += count * points->second;
    }

    const auto score = base * rate->second / 100;
    reply["action"] = score >= minimum ? "fight" : "retry_run";
    reply["score"] = score;
    reply["minimum_score"] = minimum;
    return reply;
}
} // namespace labyrinth
