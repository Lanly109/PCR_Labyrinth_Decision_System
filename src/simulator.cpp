#include "labyrinth.hpp"
#include "httplib.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>

namespace labyrinth {
namespace {
using Clock = std::chrono::steady_clock;

Party canonical(Party units) {
    std::ranges::sort(units);
    return units;
}

Json simulation_context(const Request& request, const Catalog& catalog) {
    const auto quest_id = request.body.at("quest_id").get<Id>();
    const auto& quest = catalog.quests.at(quest_id);
    const auto& enemies = catalog.wave_enemies.at(quest.wave_group_id);
    if (enemies.empty()) throw std::runtime_error("游戏数据中缺少关卡敌人");

    const auto& training = request.body.at("account_training");
    Json levels = Json::object(), talent_nodes = Json::object(), master_nodes = Json::object();
    Json mastery = Json::object();
    for (const auto& talent : training.at("talent_levels"))
        levels[std::to_string(talent.at("talent_id").get<int>())] = talent.at("level").get<int>();
    for (const auto& node : training.at("talent_skill_nodes"))
        talent_nodes[std::to_string(node.at("node_id").get<int>())] = node.at("enhance_level").get<int>();
    if (training.contains("master_skill_node")) {
        const auto& node = training.at("master_skill_node");
        master_nodes[std::to_string(node.at("node_id").get<int>())] = node.at("enhance_level").get<int>();
    }
    for (const auto& role : training.at("role_mastery")) {
        const auto& ids = catalog.role_mastery_ids.at(role.at("unit_role_id").get<int>());
        const auto& slots = role.at("slot_levels");
        const auto& enhanced = role.at("enhance_levels");
        if (slots.size() != ids.size() || enhanced.size() != ids.size())
            throw std::runtime_error("职能练度应包含四个槽位");
        for (size_t slot = 0; slot < ids.size(); ++slot) {
            if (ids[slot] <= 0) throw std::runtime_error("游戏数据中缺少职能槽位 ID");
            mastery[std::to_string(ids[slot])] = {
                {"Item1", slots.at(slot).get<int>()}, {"Item2", enhanced.at(slot).get<int>()}};
        }
    }
    const auto& reinforcement = training.at("labyrinth_enhanced_node_ids");
    if (!reinforcement.is_array()) throw std::runtime_error("黎明界强化节点应为列表");

    return {
        {"GuildEnemyList", enemies}, {"EnemyList", Json::array()},
        {"WaveGroupId", quest.wave_group_id}, {"BattleTime", quest.limit_time},
        {"LabyrinthQuestId", quest_id}, {"LabyrinthDifficulty", request.difficulty},
        {"LabyrinthRelicIds", request.owned_relics}, {"LabyrinthOwnedUnitIds", request.owned_units},
        {"LabyrinthAlphaRupee", request.gold}, {"LabyrinthEnhancedNodeIds", reinforcement},
        {"TalentSkillLevels", std::move(levels)}, {"TalentSkillNodeLevels", std::move(talent_nodes)},
        {"MasterSkillNodeLevels", std::move(master_nodes)}, {"RoleMastery", std::move(mastery)},
        {"ToMaxUnitAttr", true}, {"TalentFullInfo", false}, {"IsGuildBattle", true},
        {"IsAutoMode", true}, {"IsSetMode", false}, {"IsSemanMode", false},
        {"UBExecTimeList", Json::array()}, {"SemanUBExecTimeList", Json::array()},
        {"SetToggles", {false, false, false, false, false}},
        {"Settings", {{"UseFixedRandomSeed", false}}},
        {"IsSkipping", true}, {"CalcStats", 0}, {"NeedOnceResult", true}, {"RepeatCount", 1}
    };
}

struct Outcome {
    bool victory = false;
    int result = -1;
    int returned_seconds = 0;
    int remaining_seconds = 0;
    int frames = 0;
    std::string error;
};

int integer_field(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<int>::max())
        throw std::runtime_error(std::string("模拟器整数结果无效 ") + key);
    return value.get<int>();
}

Outcome read_outcome(const Json& response, int battle_seconds) {
    Outcome outcome;
    try {
        if (!response.at("Success").get<bool>())
            throw std::runtime_error(response.value("Message", std::string("模拟器未返回有效结果")));
        if (response.contains("Validation") && !response.at("Validation").is_null() &&
            !response.at("Validation").value("Ok", true))
            throw std::runtime_error("模拟器结果校验失败");
        outcome.result = integer_field(response, "Result");
        outcome.frames = integer_field(response, "BattleFrames");
        if (outcome.result > 2)
            throw std::runtime_error("模拟器胜负或战斗帧数无效");
        constexpr int frames_per_second = 60;
        outcome.remaining_seconds = std::max(0, battle_seconds - outcome.frames / frames_per_second);
        if (outcome.result == 0) {
            const auto& once = response.at("OnceResult");
            outcome.returned_seconds = integer_field(once, "backTime");
            if (integer_field(once, "BattleResult") != 0)
                throw std::runtime_error("模拟器返秒结果无效");
            outcome.victory = true;
        }
    } catch (const std::exception& error) {
        outcome.error = error.what();
    }
    return outcome;
}

class MzqSimulator final : public TeamSimulator {
public:
    explicit MzqSimulator(const SimulatorOptions& options)
        : deadline_(Clock::now() + std::chrono::seconds(90)) {
        const auto split = options.url.find('/', options.url.find("://") + 3);
        const auto origin = options.url.substr(0, split);
        route_ = split == std::string::npos || options.url.substr(split) == "/"
            ? "/simulate" : options.url.substr(split);
        client_ = std::make_unique<httplib::Client>(origin);
        client_->set_connection_timeout(3);
        client_->set_write_timeout(10);
        client_->set_keep_alive(true);
    }

    void rank(const Request& request, const Catalog& catalog,
              std::vector<RankedParty>& parties, Json* diagnostics) override {
        const auto started = Clock::now();
        std::vector<Party> pending;
        for (const auto& party : parties) {
            auto key = canonical(party.units);
            if (!outcomes_.contains(key)) pending.push_back(std::move(key));
        }
        if (!pending.empty() && error_.empty()) simulate(request, catalog, pending);
        std::stable_sort(parties.begin(), parties.end(), [&](const auto& left, const auto& right) {
            const auto a = outcomes_.find(canonical(left.units));
            const auto b = outcomes_.find(canonical(right.units));
            const bool win_a = a != outcomes_.end() && a->second.victory;
            const bool win_b = b != outcomes_.end() && b->second.victory;
            if (win_a != win_b) return win_a;
            return win_a && a->second.returned_seconds > b->second.returned_seconds;
        });
        elapsed_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (diagnostics) write_diagnostics(*diagnostics);
    }

private:
    void simulate(const Request& request, const Catalog& catalog, const std::vector<Party>& pending) {
        try {
            const auto context = simulation_context(request, catalog);
            Json batch = Json::array();
            for (const auto& party : pending) {
                auto body = context;
                auto& players = body["PlayerList"] = Json::array();
                for (const auto unit : party) players.push_back({{"unitId", unit}});
                batch.push_back(std::move(body));
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline_ - Clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("模拟器验证已超时");
            client_->set_read_timeout(static_cast<time_t>(remaining));
            ++batches_;
            const auto response = client_->Post(route_, batch.dump(), "application/json");
            if (!response) throw std::runtime_error("模拟器连接或读取失败 " + httplib::to_string(response.error()));
            if (response->status != 200)
                throw std::runtime_error("模拟器 HTTP " + std::to_string(response->status));
            const auto results = Json::parse(response->body);
            if (!results.is_array() || results.size() != pending.size())
                throw std::runtime_error("模拟器返回的结果数量与候选队伍数量不一致");
            for (size_t index = 0; index < pending.size(); ++index)
                outcomes_.emplace(pending[index], read_outcome(results.at(index), context.at("BattleTime").get<int>()));
        } catch (const std::exception& error) {
            error_ = error.what();
            for (const auto& party : pending) {
                Outcome outcome;
                outcome.error = error_;
                outcomes_.emplace(party, std::move(outcome));
            }
        }
    }

    void write_diagnostics(Json& diagnostics) const {
        Json candidates = Json::array();
        size_t wins = 0;
        for (const auto& [units, outcome] : outcomes_) {
            wins += outcome.victory;
            Json row = {{"units", units}, {"victory", outcome.victory}, {"result", outcome.result},
                        {"battle_frames", outcome.frames}, {"back_time", outcome.returned_seconds}};
            if (outcome.error.empty()) row["remaining_seconds"] = outcome.remaining_seconds;
            if (!outcome.error.empty()) row["error"] = outcome.error;
            candidates.push_back(std::move(row));
        }
        auto& record = diagnostics["simulator"];
        record = {{"candidate_count", outcomes_.size()}, {"win_count", wins}, {"batches", batches_},
                  {"elapsed_ms", elapsed_ms_}, {"fallback", wins == 0}, {"candidates", std::move(candidates)}};
        if (!error_.empty()) record["error"] = error_;
    }

    Clock::time_point deadline_;
    std::unique_ptr<httplib::Client> client_;
    std::string route_, error_;
    std::map<Party, Outcome> outcomes_;
    int batches_ = 0;
    double elapsed_ms_ = 0;
};
} // namespace

std::unique_ptr<TeamSimulator> make_team_simulator(const SimulatorOptions& options) {
    return std::make_unique<MzqSimulator>(options);
}
} // namespace labyrinth
