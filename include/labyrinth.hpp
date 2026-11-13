#pragma once

#include "json.hpp"
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace labyrinth {
using Json = nlohmann::json;
using Id = int;
using Ids = std::vector<Id>;
using Party = std::vector<Id>;
using Formation = std::vector<Party>;

enum class Kind { select_ticket, unit_choice, ticket_choice, relic_choice, event, shop, team, opening, path, score_check, ticket_clear };

struct BadRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct OpeningBlock {
    Id id;
    int region, column, type;
    Ids next, boss_units;
    bool cleared = false;
};

struct Request {
    Kind kind;
    std::string request_id;
    Id enter_id, region, block_id, difficulty, gold;
    Ids owned_units, owned_relics;
    Json body;
    int opening_attempt = 0;
    std::vector<OpeningBlock> opening_blocks{};
    std::unordered_set<Id> npc_units{};
};

inline std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

inline std::string path_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

Json read_json(const std::filesystem::path& path);
Request parse_request(Json body);

inline constexpr int shop_max_items = 9;
inline constexpr int shop_imprint_slots = 3;
inline constexpr int shop_max_relic_slots = shop_max_items - shop_imprint_slots;

struct ShopForecast {
    int random_slots, choice_slots, relic_slots;
    std::array<std::array<double, 3>, 3> relic_star_probabilities;
    Ids fixed_relic_ids;
};
struct ShopModel {
    int random_imprint_price, choice_imprint_price;
    std::array<int, 3> relic_prices;
    std::array<int, 2> refresh_fees;
    int imprint_score, relic_score;
    std::unordered_map<int, ShopForecast> regions;
};
struct ShopRegion {
    bool purchase = false;
    int reserve_gold = 0;
};
struct ShopPolicy {
    bool enabled = false;
    std::unordered_map<int, ShopRegion> regions;
    ShopModel model{};
};
inline constexpr int unit_role_count = 8;
struct RecruitBossTarget {
    int weight, parties;
};
struct RecruitPolicy {
    bool enabled = false;
    int difficulty = 0;
    std::map<int, RecruitBossTarget> boss_targets;
    std::unordered_map<Id, int> ex_weights;
};
std::string recruit_boss_label(int region, int parties);
struct ModelOptions {
    std::filesystem::path directory = "models";
    int threads = 4, candidates = 64;
    bool use_gpu = false;
    std::optional<std::string> gpu_adapter;
    uint64_t candidate_seed = 20261101;

    bool operator==(const ModelOptions&) const = default;
};
// Indexed by the native block type and one-based region.
using OpeningMinimums = std::array<int, 9>;
inline constexpr std::array<const char*, 7> opening_cell_names{
    "normal", "hard", "ticket", "event", "relic", "shop", "boss"
};

struct OpeningPolicy {
    bool enabled = false;
    int max_attempts = 100;
    std::array<OpeningMinimums, 6> regions{};
    std::array<Ids, 6> boss_units{};
};

inline constexpr size_t opening_pick_count = 3;
inline constexpr bool is_fes_unit(Id id) { return id >= 180000; }

struct OpeningTicket {
    Id ticket_id, guild_id;
    Ids default_units;
};
std::vector<OpeningTicket> parse_opening_tickets(const Json& catalog);

struct SelectTicketRoster {
    Id ticket_id;
    std::string label;
    Ids unit_ids;
};
std::vector<SelectTicketRoster> parse_select_ticket_rosters(const Json& catalog);

inline constexpr std::array<int, 4> log_history_limits{500, 1000, 2000, 0};

inline constexpr int default_passport_reserve = 0;
inline constexpr Id default_ticket_clear_guild = 3;

inline constexpr const char* default_simulator_url = "http://127.0.0.1:8080/simulate";
struct SimulatorOptions {
    bool enabled = false;
    std::string url = default_simulator_url;
};

struct Config {
    int log_history_limit = log_history_limits.front();
    std::string host = "127.0.0.1";
    int port = 8081;
    std::filesystem::path catalog = "data/game_catalog.json";
    ModelOptions model;
    SimulatorOptions simulator;
    bool opening_ticket = false, select_ticket = false, ticket_choice = false, relic_choice = false;
    bool event = false, team = false;
    bool team_ignore_npc = false;
    int third_boss_party_count = 3;
    int battle_max_retries = 4;
    bool restart_at_battle_retry_limit = false;
    std::array<bool, 3> team_scopes{true, true, true};
    Ids talent_priority, mark_priority;
    std::unordered_map<Id, Ids> opening_ticket_units, ticket_priority_units;
    RecruitPolicy recruitment;
    std::unordered_map<Id, int> fixed_event_choices;
    ShopPolicy shop;
    OpeningPolicy opening;
    bool path = false, score_farm = false;
    std::unordered_map<Id, int> guild_minimum_scores;
    int passport_reserve = default_passport_reserve;
    Ids ticket_clear_guilds{default_ticket_clear_guild};
};
Config parse_config(const Json& data);

struct Relic {
    int rarity, mark, mark_count;
    std::string name;
};
struct Ticket {
    int summon_type, summon_num, price;
    std::string name;
};
struct Quest {
    int type, wave_group_id, limit_time;
};
struct EventOption {
    int condition_type, condition_value;
    Id unit_id;
};
struct RecruitExTarget {
    std::string name;
    Ids quest_ids;
};
std::vector<RecruitExTarget> parse_recruit_ex_targets(const Json& catalog);

struct RecruitCatalog {
    std::vector<RecruitExTarget> ex_targets;
    std::unordered_set<Id> healing_relics;
    int guard_mark_threshold = 0;
};
struct Catalog {
    RecruitCatalog recruitment;
    std::unordered_map<Id, int> unit_roles;
    std::unordered_map<Id, Relic> relics;
    std::unordered_map<Id, Ticket> tickets;
    std::vector<OpeningTicket> opening_tickets;
    std::unordered_set<Id> guild_ids;
    std::unordered_map<Id, std::string> guild_names;
    std::vector<SelectTicketRoster> select_ticket_rosters;
    std::unordered_map<Id, Quest> quests;
    std::unordered_map<Id, Ids> wave_enemies;
    std::map<int, std::array<Id, 4>> role_mastery_ids;
    std::unordered_map<Id, std::array<Id, 3>> events;
    std::unordered_map<Id, EventOption> event_options;
    std::unordered_map<int, int> score_settings, score_rates;
};
Catalog parse_catalog(const Json& data);
void validate_ticket_pools(const Config& config, const Catalog& catalog);

struct RankedParty {
    Party units;
    double win_probability;
};

class TeamSimulator {
public:
    virtual ~TeamSimulator() = default;
    virtual void rank(const Request& request, const Catalog& catalog,
                      std::vector<RankedParty>& parties, Json* diagnostics) = 0;
};
std::unique_ptr<TeamSimulator> make_team_simulator(const SimulatorOptions& options);

struct TeamPlan {
    Formation parties;
    std::vector<std::optional<double>> win_probabilities;
};

enum class TeamPlanFailure { unsupported, no_candidates };
using TeamPlanResult = std::variant<TeamPlan, TeamPlanFailure>;

class TeamModel {
public:
    virtual ~TeamModel() = default;
    virtual std::string inference_backend() const { return "CPU"; }
    virtual bool supports_quest(Id quest) const = 0;
    virtual bool supports_unit(Id unit) const = 0;
    virtual std::vector<RankedParty> ranked(Id quest, const Ids& owned, const Ids& relics,
                                           int seconds, size_t limit) = 0;
    virtual std::vector<RankedParty> recruit_seeds(Id, const Ids&, const Ids&, int) { return {}; }
    virtual std::vector<RankedParty> recruit_scores(Id, const std::vector<Party>&, const Ids&, int) { return {}; }
};
std::unique_ptr<TeamModel> load_model(const ModelOptions& options);

Json decide(const Request& request, const Config& config, const Catalog& catalog, TeamModel* model,
            Json* diagnostics = nullptr, TeamSimulator* simulator = nullptr);
int plan_recruitment(const Request& request, const RecruitPolicy& policy, const Catalog& catalog,
                     TeamModel* model, Json* diagnostics);
Json plan_opening(const Request& request, const OpeningPolicy& policy, Json* diagnostics = nullptr);
Json plan_path(const Request& request, const OpeningPolicy& policy, Json* diagnostics = nullptr);
Json check_score(const Request& request, const Config& config, const Catalog& catalog);
Json plan_shop(const Request& request, const ShopPolicy& policy, const Catalog& catalog);
TeamPlanResult plan_team(const Request& request, int max_attempts,
                         const Catalog& catalog, TeamModel& model, bool ignore_npc = false,
                         TeamSimulator* simulator = nullptr, Json* diagnostics = nullptr,
                         int third_boss_party_count = 3);
} // namespace labyrinth
