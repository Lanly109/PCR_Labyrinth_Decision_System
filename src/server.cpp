#include "server_runtime.hpp"
#include "httplib.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace labyrinth {

struct ServerState {
    struct Model {
        explicit Model(std::unique_ptr<TeamModel> loaded)
            : inference(std::move(loaded)), backend(inference->inference_backend()) {}
        std::unique_ptr<TeamModel> inference;
        const std::string backend;
        std::mutex mutex;
    };

    Config config;
    Catalog catalog;
    std::shared_ptr<Model> model;
};

namespace {
class SynchronizedTeamModel final : public TeamModel {
public:
    explicit SynchronizedTeamModel(ServerState::Model& model) : model_(model) {}
    bool supports_quest(Id id) const override { return model_.inference->supports_quest(id); }
    bool supports_unit(Id id) const override { return model_.inference->supports_unit(id); }
    std::vector<RankedParty> ranked(Id quest, const Ids& owned, const Ids& relics,
                                   int seconds, size_t limit) override {
        std::scoped_lock lock(model_.mutex);
        return model_.inference->ranked(quest, owned, relics, seconds, limit);
    }
private:
    ServerState::Model& model_;
};

namespace fs = std::filesystem;
using DecisionClock = std::chrono::steady_clock;

constexpr const char* endpoint = "/labyrinth/decision";
constexpr const char* screenshot_endpoint = "/labyrinth/result-screenshot";
constexpr size_t max_payload = 1024 * 1024;
constexpr size_t max_screenshot_bytes = 16 * max_payload;
std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto whole_seconds = floor<seconds>(now);
    const auto time = system_clock::to_time_t(whole_seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream formatted;
    formatted << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3)
              << duration_cast<milliseconds>(now - whole_seconds).count() << 'Z';
    return formatted.str();
}

class LogSink {
public:
    explicit LogSink(const ServerRunOptions& options) : options_(options) {}

    void open_text(const fs::path& path) {
        text_.open(path, std::ios::app);
        if (!text_) throw std::runtime_error("Cannot open log file: " + path_utf8(path));
    }

    void open_jsonl(const fs::path& path) {
        if (path.empty()) return;
        if (path.has_parent_path()) fs::create_directories(path.parent_path());
        jsonl_.open(path, std::ios::app);
        if (!jsonl_) throw std::runtime_error("Cannot open decision log: " + path_utf8(path));
    }

    void notice(std::string message) {
        {
            std::scoped_lock lock(mutex_);
            print(timestamp() + " [INFO] " + message);
        }
        if (options_.notice) options_.notice(std::move(message));
    }

    void decision(DecisionClock::time_point start, const std::string& uid, const Json& request,
                  const httplib::Response& response, const char* reason = nullptr,
                  const Json& diagnostics = Json()) {
        const auto elapsed = std::chrono::duration<double, std::milli>(DecisionClock::now() - start).count();
        auto field = [&request](const char* name) -> Json {
            return request.is_object() && request.contains(name) ? request.at(name) : Json(nullptr);
        };
        const auto status = response.status;
        const auto stamp = timestamp();
        Json record = {{"timestamp", stamp}, {"uid", uid.empty() ? Json(nullptr) : Json(uid)},
                       {"request_id", field("request_id")}, {"kind", field("kind")},
                       {"http_status", status}, {"elapsed_ms", elapsed}, {"request", request},
                       {"response", Json::parse(response.body)}};
        const auto account_name = field("account_name");
        if (!uid.empty() && account_name.is_string()) {
            const auto& name = account_name.get_ref<const std::string&>();
            if (name.size() <= 128 && name.find_first_not_of(' ') != std::string::npos &&
                std::ranges::none_of(name, [](unsigned char ch) { return ch < 32 || ch == 127; }))
                record["account_name"] = account_name;
        }
        if (reason) record["reason"] = reason;
        if (!diagnostics.is_null()) record["diagnostics"] = diagnostics;
        std::ostringstream line;
        line << stamp << (status >= 500 ? " [ERROR]" : status >= 400 ? " [WARN]" : " [INFO]")
             << " decision kind=" << field("kind") << " uid=" << record.at("uid");
        if (record.contains("account_name")) line << " account_name=" << record.at("account_name");
        line << " request_id=" << record.at("request_id") << " region=" << field("region")
             << " status=" << status << " elapsed_ms=" << std::fixed << std::setprecision(3) << elapsed
             << " reply=" << response.body;
        if (reason) line << " reason=" << reason;
        std::scoped_lock lock(mutex_);
        print(line.str());
        if (jsonl_.is_open() || options_.decision_log) {
            const auto serialized = record.dump();
            std::streamoff file_end = 0;
            if (jsonl_.is_open()) {
                jsonl_ << serialized << std::endl;
                if (jsonl_) file_end = jsonl_.tellp();
            }
            if (options_.decision_log)
                options_.decision_log(record, serialized.size(), std::max<std::streamoff>(0, file_end));
        }
    }

private:
    void print(const std::string& line) {
        std::cerr << line << '\n';
        if (text_) text_ << line << std::endl;
    }

    const ServerRunOptions& options_;
    std::mutex mutex_;
    std::ofstream text_, jsonl_;
};

} // namespace

fs::path executable_dir(const char*) {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("Cannot locate executable");
    return fs::path(std::wstring(path.data(), size)).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size)) throw std::runtime_error("Cannot locate executable");
    return fs::canonical(path.data()).parent_path();
#else
    return fs::canonical("/proc/self/exe").parent_path();
#endif
}

std::shared_ptr<const ServerState> ServerControl::snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::string ServerControl::backend() const {
    const auto state = snapshot();
    return state && state->model ? state->model->backend : "Disabled";
}

void ServerControl::stop() {
    std::scoped_lock lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    ready_.store(false);
    if (server_) server_->stop();
}

namespace {

std::shared_ptr<const ServerState> make_state(Config config, Catalog catalog,
                                              [[maybe_unused]] const ServerState* previous = nullptr) {
    validate_ticket_pools(config, catalog);
    if (config.recruitment.enabled && catalog.unit_roles.empty())
        throw std::runtime_error("角色选择需要包含角色职能的游戏数据");
    const auto& ex_targets = catalog.recruitment.ex_targets;
    for (const auto& [quest, weight] : config.recruitment.ex_weights) {
        if (std::ranges::none_of(ex_targets, [quest](const auto& target) {
                return target.quest_ids.front() == quest;
            }))
            throw std::runtime_error("无效的 EX 权重键，应使用该遭遇最低楼层的关卡 ID：" + std::to_string(quest));
    }

    std::shared_ptr<ServerState::Model> model;
    if (config.team || config.recruitment.enabled) {
#ifdef LABYRINTH_WITH_ONNX
        if (previous && previous->model && previous->config.model == config.model) {
            model = previous->model;
        } else {
            try {
                model = std::make_shared<ServerState::Model>(load_model(config.model));
            } catch (const std::exception& error) {
                throw std::runtime_error("Cannot load team model from " +
                                         path_utf8(config.model.directory) + ": " + error.what());
            }
        }
#else
        throw std::runtime_error(config.recruitment.enabled ?
            "新版角色选择需要启用 LABYRINTH_WITH_ONNX" :
            "policies.team.enabled requires a build with LABYRINTH_WITH_ONNX=ON");
#endif
    }
    return std::make_shared<const ServerState>(ServerState{
        std::move(config), std::move(catalog), std::move(model)
    });
}

template<class Integer>
std::optional<Integer> decimal(const std::string& value) {
    Integer n{};
    const auto* end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, n);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return n;
}

void reply(httplib::Response& res, int status, const Json& json) {
    res.status = status;
    res.set_content(json.dump(), "application/json; charset=utf-8");
}

std::string screenshot_guild_name(const Catalog& catalog, Id id) {
    const auto entry = catalog.guild_names.find(id);
    auto name = entry == catalog.guild_names.end() ? std::string{} : entry->second;
    for (auto pos = name.find("\\n"); pos != std::string::npos; pos = name.find("\\n", pos))
        name.replace(pos, 2, " ");
    for (char& ch : name)
        if (static_cast<unsigned char>(ch) < 32 || std::string_view("/\\:*?\"<>|").find(ch) != std::string_view::npos)
            ch = '_';
    return name.empty() || name.size() > 160 ? "公会" + std::to_string(id) : name;
}

void save_screenshot(const httplib::Request& request, httplib::Response& response, const Catalog& catalog,
                     const fs::path& directory, std::mutex& mutex) {
    const auto parameter = [&](const char* key) {
        return request.get_param_value_count(key) == 1 ? decimal<int>(request.get_param_value(key)) : std::nullopt;
    };
    const auto guild = parameter("guild_id");
    const auto units = parameter("unit_count");
    const auto relics = parameter("relic_count");
    const auto score = parameter("score");
    if (!guild || !catalog.guild_ids.contains(*guild) || !units || *units < 0 ||
        !relics || *relics < 0 || !score || *score < 0) {
        reply(response, 400, {{"error", "Invalid result screenshot metadata"}});
        return;
    }

    constexpr std::string_view signature = "\x89PNG\r\n\x1a\n";
    constexpr std::string_view end_chunk = {"\0\0\0\0IEND\xae\x42\x60\x82", 12};
    const auto& png = request.body;
    if (png.size() < 45 || !png.starts_with(signature) || png.compare(12, 4, "IHDR") != 0 ||
        !png.ends_with(end_chunk)) {
        reply(response, 400, {{"error", "Expected a PNG screenshot"}});
        return;
    }

    auto time = timestamp();
    std::ranges::replace(time, ':', '-');
    const auto stem = screenshot_guild_name(catalog, *guild) + "_角色" + std::to_string(*units) +
        "_遗物" + std::to_string(*relics) + "_" + std::to_string(*score) + "分_" + time;
    std::scoped_lock lock(mutex);
    fs::create_directories(directory);
    auto filename = stem + ".png";
    auto destination = directory / path_from_utf8(filename);
    for (int suffix = 1; fs::exists(destination); ++suffix) {
        filename = stem + "_" + std::to_string(suffix) + ".png";
        destination = directory / path_from_utf8(filename);
    }

    auto temporary = destination;
    temporary += ".tmp";
    try {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(temporary, std::ios::binary | std::ios::trunc);
        file.write(png.data(), static_cast<std::streamsize>(png.size()));
        file.close();
        fs::rename(temporary, destination);
    } catch (...) {
        std::error_code error;
        fs::remove(temporary, error);
        throw;
    }
    reply(response, 201, {{"filename", filename}});
}

void run_self_test(const Config& config, const Catalog& catalog, TeamModel* model) {
    if (!model) throw std::runtime_error("--self-test requires policies.team.enabled");
    const auto metadata = read_json(config.model.directory / "metadata.json");
    Json checks = Json::array();
    for (auto quest : {770331601, 770531601, 770520101}) {
        auto parties = catalog.quests.at(quest).type == 3 ? 3 : 1;
        auto req = parse_request({{"protocol_version", 1}, {"request_id", "self-test"},
            {"kind", "team"}, {"enter_id", 12}, {"region", 3}, {"block_id", 0}, {"difficulty", 5},
            {"gold", 0}, {"owned_unit_ids", metadata.at("unit_ids")}, {"owned_relic_ids", Json::array()},
            {"quest_id", quest}, {"party_count", parties}, {"past_attempts", Json::array()}});
        auto result = plan_team(req, 8, catalog, *model);
        const auto* plan = std::get_if<TeamPlan>(&result);
        if (!plan || plan->parties.size() != static_cast<size_t>(parties))
            throw std::runtime_error("Self-test could not form teams for " + std::to_string(quest));
        checks.push_back({{"quest_id", quest}, {"parties", plan->parties}});
    }
    std::cout << Json({{"status", "passed"}, {"results", checks}}).dump() << '\n';
}

} // namespace

void ServerControl::reload(const fs::path& root, Config config, Catalog catalog,
                           const std::function<void()>& persist) {
    const auto previous = snapshot();
    if (!ready() || !previous) throw std::runtime_error("服务已停止，请重新保存");
    if (config.host != previous->config.host || config.port != previous->config.port)
        throw std::runtime_error("监听地址或端口变更需要重启服务");

    config.model.directory = root / config.model.directory;
    auto next = make_state(std::move(config), std::move(catalog), previous.get());
    std::scoped_lock lock(mutex_);
    if (!ready() || state_ != previous) throw std::runtime_error("服务配置已变更，请重新保存");
    persist();
    state_ = std::move(next);
}

int run_server(const std::vector<std::string>& args, const ServerRunOptions& options, ServerControl* control) {
    const auto root = options.root.empty() ? executable_dir() : options.root;
    fs::path config_path = options.config_path.empty() ? root / "config/labyrinth.json" : options.config_path;
    fs::path catalog_override, model_override, decision_log_path = options.decision_log_path;
    LogSink logs(options);
    std::optional<std::string> host;
    std::optional<int> port;
    bool self_test = false;
    for (size_t i = 0; i < args.size(); ++i) {
        auto flag = args[i];
        if (flag == "--help" || flag == "-h") {
            std::cout << "Labyrinth decision server\n"
                "  --config PATH --catalog PATH --models-dir PATH --host ADDRESS --port PORT\n"
                "  --log-file PATH --decision-log PATH --self-test\n";
            return 0;
        }
        if (flag == "--self-test") {
            self_test = true;
            continue;
        }
        if (i + 1 == args.size()) throw std::runtime_error("Missing value for " + flag);
        const auto& value = args[++i];
        if (flag == "--config") config_path = path_from_utf8(value);
        else if (flag == "--catalog") catalog_override = path_from_utf8(value);
        else if (flag == "--models-dir") model_override = path_from_utf8(value);
        else if (flag == "--host") host = value;
        else if (flag == "--port") {
            port = decimal<int>(value);
            if (!port || *port < 1 || *port > 65535) throw std::runtime_error("Invalid port");
        } else if (flag == "--log-file") logs.open_text(path_from_utf8(value));
        else if (flag == "--decision-log") decision_log_path = path_from_utf8(value);
        else throw std::runtime_error("Unknown option " + flag);
    }
    auto config = parse_config(read_json(config_path));
    if (host) config.host = *host;
    if (port) config.port = *port;
    config.catalog = catalog_override.empty() ? root / config.catalog : catalog_override;
    config.model.directory = model_override.empty() ? root / config.model.directory : model_override;
    auto catalog = parse_catalog(read_json(config.catalog));
    const auto listen_host = config.host;
    const auto listen_port = config.port;
    auto state = make_state(std::move(config), std::move(catalog));
    if (self_test) {
        run_self_test(state->config, state->catalog, state->model ? state->model->inference.get() : nullptr);
        return 0;
    }

    if (control) {
        std::scoped_lock lock(control->mutex_);
        if (control->stopping_) return 0;
        control->state_ = std::move(state);
    }
    logs.open_jsonl(decision_log_path);

    httplib::Server server;
    server.new_task_queue = [&] {
        auto queue = std::make_unique<httplib::ThreadPool>(4);
        // httplib sets is_running() before calling this factory.
        if (control) {
            std::scoped_lock lock(control->mutex_);
            control->server_ = &server;
            if (control->stopping_) server.stop();
            else control->ready_.store(true);
        }
        return queue.release();
    };
    server.set_keep_alive_max_count(1);
    server.set_payload_max_length(max_screenshot_bytes);
    const auto screenshot_directory = config_path.parent_path() / "screenshots";
    std::mutex screenshot_mutex;
    const auto request_uid = [](const httplib::Request& request) -> std::string {
        if (request.get_header_value_count("uid") != 1) return {};
        const auto header = request.get_header_value("uid");
        const auto uid = decimal<int64_t>(header);
        return uid && *uid > 0 ? std::to_string(*uid) : std::string{};
    };
    server.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        const bool screenshot = req.path == screenshot_endpoint;
        if (req.method != "POST" || (!screenshot && req.path != endpoint))
            return httplib::Server::HandlerResponse::Unhandled;
        const auto started = DecisionClock::now();
        const auto uid = request_uid(req);
        auto length = decimal<size_t>(req.get_header_value("Content-Length"));
        if (req.get_header_value_count("Content-Length") != 1 || !length || *length == 0 ||
            *length > (screenshot ? max_screenshot_bytes : max_payload) ||
            req.has_header("Transfer-Encoding") || req.has_header("Content-Encoding") || req.has_header("Range")) {
            reply(res, 400, {{"error", screenshot ? "Expected raw PNG with one valid Content-Length" :
                                                    "Expected plain JSON with one valid Content-Length"}});
            if (!screenshot) logs.decision(started, uid, nullptr, res, "invalid_request");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (uid.empty()) {
            reply(res, 400, {{"error", "uid must be the positive character ViewerId"}});
            if (!screenshot) logs.decision(started, uid, nullptr, res, "invalid_request");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (screenshot && (req.get_header_value_count("Content-Type") != 1 ||
                           req.get_header_value("Content-Type") != "image/png")) {
            reply(res, 415, {{"error", "Expected image/png"}});
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Post(screenshot_endpoint, [&](const httplib::Request& incoming, httplib::Response& outgoing) {
        const auto active = control ? control->snapshot() : state;
        try {
            save_screenshot(incoming, outgoing, active->catalog, screenshot_directory, screenshot_mutex);
        } catch (const std::exception&) {
            reply(outgoing, 500, {{"error", "Could not save the result screenshot"}});
        }
    });
    server.Post(endpoint, [&](const httplib::Request& incoming, httplib::Response& outgoing) {
        const auto started = DecisionClock::now();
        const auto active = control ? control->snapshot() : state;
        const auto uid = request_uid(incoming);
        Json body;
        std::optional<Request> req;
        try {
            body = Json::parse(incoming.body);
            req = parse_request(body);
        } catch (const BadRequest& e) {
            reply(outgoing, 400, {{"error", e.what()}});
            logs.decision(started, uid, body, outgoing, "invalid_request");
            return;
        } catch (const Json::exception&) {
            reply(outgoing, 400, {{"error", "Invalid UTF-8 JSON request"}});
            logs.decision(started, uid, body, outgoing, "invalid_request");
            return;
        }
        const auto enabled = [&] { return !control || control->enabled.load(); };
        const char* reason = nullptr;
        Json diagnostics;
        try {
            Json decision;
            if (enabled()) {
                const auto& model = active->model;
                if (req->kind == Kind::team && model) {
                    SynchronizedTeamModel inference(*model);
                    auto simulator = active->config.simulator.enabled
                        ? make_team_simulator(active->config.simulator) : nullptr;
                    if (enabled()) decision = decide(*req, active->config, active->catalog,
                                                    &inference, &diagnostics, simulator.get());
                } else {
                    std::unique_lock<std::mutex> lock;
                    const bool recruitment = req->kind == Kind::unit_choice && active->config.recruitment.enabled;
                    if (recruitment && model) lock = std::unique_lock(model->mutex, std::try_to_lock);
                    if (enabled()) decision = decide(*req, active->config, active->catalog,
                                                    lock.owns_lock() ? model->inference.get() : nullptr, &diagnostics);
                }
            }
            if (decision.is_null() || !enabled()) {
                decision = {{"request_id", req->request_id}, {"action", "noop"}};
                reason = "master_disabled";
                diagnostics = nullptr;
            } else if (decision.contains("defeat_count")) {
                reason = diagnostics.contains("battle_retry_no_team") ? "battle_retry_no_team" : "battle_retry_limit";
            }
            reply(outgoing, 200, decision);
        } catch (const std::exception&) {
            reply(outgoing, 500, {{"request_id", req->request_id}, {"error", "Internal decision error"}});
            reason = "decision_error";
            diagnostics = nullptr;
        }
        logs.decision(started, uid, req->body, outgoing, reason, diagnostics);
    });
    if (!server.bind_to_port(listen_host, listen_port)) throw std::runtime_error("Cannot bind labyrinth server");
    logs.notice("Listening on " + listen_host + ":" + std::to_string(listen_port) + endpoint);
    const auto clear_listener = [&] {
        if (!control) return;
        std::scoped_lock lock(control->mutex_);
        control->server_ = nullptr;
        control->ready_.store(false);
    };
    try {
        const auto ok = server.listen_after_bind();
        clear_listener();
        return ok ? 0 : 1;
    } catch (...) {
        clear_listener();
        throw;
    }
}
} // namespace labyrinth
