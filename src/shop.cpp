#include "labyrinth.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace labyrinth {
namespace {
constexpr double epsilon = 1e-9;
constexpr int ticket_reward = 25;
constexpr int relic_reward = 26;
constexpr int purchase_available = 1;
using RelicMask = unsigned;

struct Product {
    int64_t cost;
    int score;
    RelicMask fixed_relic = 0;
};
struct Subset {
    int64_t cost = 0, score = 0;
    std::vector<size_t> indices;
    RelicMask fixed_relics = 0;
};
struct Value {
    double score = 0, spent = 0;
};

int compare(Value left, Value right) {
    if (left.score > right.score + epsilon) return 1;
    if (right.score > left.score + epsilon) return -1;
    if (left.spent < right.spent - epsilon) return 1;
    if (right.spent < left.spent - epsilon) return -1;
    return 0;
}

struct Plan {
    Value value{-std::numeric_limits<double>::infinity(), 0};
    std::vector<size_t> indices;
    bool refresh = false;
};

std::vector<Subset> subsets(const std::vector<Product>& products) {
    std::vector<Subset> result(1);
    for (size_t index = 0; index < products.size(); ++index) {
        const auto& product = products[index];
        auto size = result.size();
        for (size_t i = 0; i < size; ++i) {
            if (result[i].fixed_relics & product.fixed_relic) continue;
            auto picked = result[i];
            picked.cost += product.cost;
            picked.score += product.score;
            picked.indices.push_back(index);
            picked.fixed_relics |= product.fixed_relic;
            result.push_back(std::move(picked));
        }
    }
    return result;
}

struct RelicDraw {
    std::array<int, 3> stars;
    double probability;
};

// Replace this distribution if observations establish a joint page model.
std::vector<RelicDraw> relic_draws(int slots, const std::array<double, 3>& p) {
    std::array<double, shop_max_relic_slots + 1> factorial{1};
    for (int i = 1; i <= slots; ++i) factorial[i] = factorial[i - 1] * i;
    std::vector<RelicDraw> draws;
    for (int a = 0; a <= slots; ++a) {
        for (int b = 0; b <= slots - a; ++b) {
            int c = slots - a - b;
            double probability = factorial[slots] / (factorial[a] * factorial[b] * factorial[c])
                * std::pow(p[0], a) * std::pow(p[1], b) * std::pow(p[2], c);
            draws.push_back({{a, b, c}, probability});
        }
    }
    return draws;
}

double terminal_value(int64_t /*unspent_gold*/) { return 0.0; }

class Planner {
    struct Page {
        double probability;
        std::vector<Subset> options;
    };
    const ShopModel& model_;
    int last_reset_;
    std::array<std::vector<Page>, 3> pages_;
    std::map<std::tuple<int, int64_t, RelicMask>, Value> expected_;

    Value expected(int reset, int64_t gold, RelicMask bought_relics) {
        auto key = std::make_tuple(reset, gold, bought_relics);
        auto cached = expected_.find(key);
        if (cached != expected_.end()) return cached->second;
        Value total;
        const auto fee = reset < last_reset_ ? model_.refresh_fees.at(reset) : 0;
        for (const auto& page : pages_.at(reset)) {
            const auto value = evaluate(page.options, reset, gold, fee, true, bought_relics).value;
            total.score += page.probability * value.score;
            total.spent += page.probability * value.spent;
        }
        expected_.emplace(key, total);
        return total;
    }

    static void offer(Plan& best, Value value, const Subset& picks, bool refresh) {
        const auto order = compare(value, best.value);
        if (order > 0 || (order == 0 && picks.indices < best.indices)) {
            best = {value, picks.indices, refresh};
        }
    }

public:
    Planner(const ShopModel& model, const ShopForecast& forecast, const std::vector<Product>& current,
            std::optional<int> random_price, int reset, int last_reset) : model_(model), last_reset_(last_reset) {
        if (!random_price) return;
        // Infer one page-wide discount from the random imprint. Future prices floor to integer gold.
        auto discounted = [&](int base) { return int64_t(base) * *random_price / model.random_imprint_price; };
        std::vector<Product> imprints(forecast.choice_slots,
            Product{discounted(model.choice_imprint_price), model.imprint_score});
        imprints.insert(imprints.end(), forecast.random_slots, Product{*random_price, model.imprint_score});

        if (!forecast.fixed_relic_ids.empty()) {
            auto products = imprints;
            for (const auto& product : current)
                if (product.fixed_relic) products.push_back(product);
            auto options = subsets(products);
            for (int next = reset + 1; next <= last_reset_; ++next)
                pages_.at(next).push_back({1.0, options});
            return;
        }

        for (int next = reset + 1; next <= last_reset_; ++next) {
            for (const auto& draw : relic_draws(forecast.relic_slots, forecast.relic_star_probabilities.at(next))) {
                if (draw.probability == 0) continue;
                auto products = imprints;
                for (size_t i = 0; i < 3; ++i)
                    products.insert(products.end(), draw.stars[i], Product{discounted(model.relic_prices[i]), model.relic_score});
                pages_.at(next).push_back({draw.probability, subsets(products)});
            }
        }
    }

    Plan evaluate(const std::vector<Subset>& options, int reset, int64_t gold, int fee, bool allow_refresh,
                  RelicMask bought_relics = 0) {
        Plan close, refresh;
        for (const auto& picks : options) {
            if (picks.cost > gold || (picks.fixed_relics & bought_relics)) continue;
            auto remaining = gold - picks.cost;
            offer(close, {picks.score + terminal_value(remaining), static_cast<double>(picks.cost)}, picks, false);
            if (allow_refresh && reset < last_reset_ && fee <= remaining) {
                const auto next_relics = bought_relics | picks.fixed_relics;
                const auto future = expected(reset + 1, remaining - fee, next_relics);
                offer(refresh, {picks.score + future.score, picks.cost + fee + future.spent}, picks, true);
            }
        }
        return compare(refresh.value, close.value) > 0 ? refresh : close;
    }
};
} // namespace

Json plan_shop(const Request& req, const ShopPolicy& policy, const Catalog& catalog) {
    auto region = policy.regions.find(req.region);
    if (region == policy.regions.end()) return {{"request_id", req.request_id}, {"action", "noop"}};
    Json reply = {{"request_id", req.request_id}, {"action", "shop_plan"},
                  {"buy_lineup_ids", Json::array()}, {"after", "close"}};
    if (!region->second.purchase || req.gold < region->second.reserve_gold) return reply;

    const auto& m = policy.model;
    const auto& forecast = m.regions.at(req.region);
    const auto& fixed_relic_ids = forecast.fixed_relic_ids;
    auto reset = req.body.at("reset_count").get<int>();
    auto limit = std::min(2, req.body.at("reset_limit").get<int>());
    auto fee = req.body.at("refresh_cost").get<int>();
    auto budget = req.gold - region->second.reserve_gold;
    Ids lineups;
    std::vector<Product> products;
    std::optional<int> random_price;
    bool consistent_discount = true;
    for (const auto& item : req.body.at("items")) {
        auto type = item.at("reward_type").get<int>();
        auto id = item.at("reward_id").get<Id>();
        auto price = item.at("price").get<int>();
        if (type == ticket_reward) {
            auto ticket = catalog.tickets.find(id);
            if (ticket != catalog.tickets.end() && ticket->second.summon_type == 3 &&
                ticket->second.price == m.random_imprint_price) {
                if (random_price && price != *random_price) consistent_discount = false;
                random_price = price;
            }
        }
        if (item.at("sold") != 0 || item.at("availability") != purchase_available) continue;
        int score = type == ticket_reward ? m.imprint_score : type == relic_reward ? m.relic_score : 0;
        if (!score) continue;
        RelicMask fixed_relic = 0;
        if (type == relic_reward) {
            const auto fixed = std::find(fixed_relic_ids.begin(), fixed_relic_ids.end(), id);
            if (fixed != fixed_relic_ids.end()) fixed_relic = 1u << (fixed - fixed_relic_ids.begin());
        }
        lineups.push_back(item.at("lineup_id").get<Id>());
        products.push_back({price, score, fixed_relic});
    }
    if (!consistent_discount) random_price.reset();
    Planner planner(m, forecast, products, random_price, std::min(reset, limit), limit);
    auto plan = planner.evaluate(subsets(products), reset, budget, fee, random_price.has_value());
    for (auto index : plan.indices) reply["buy_lineup_ids"].push_back(lineups[index]);
    if (plan.refresh) reply["after"] = "refresh";
    return reply;
}
} // namespace labyrinth
