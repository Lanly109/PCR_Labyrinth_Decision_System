#include "labyrinth.hpp"
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include "windows_gpu.hpp"
#include <windows.h>
#elif defined(__APPLE__) && __has_include("coreml_provider_factory.h")
#include "coreml_provider_factory.h"
#define LABYRINTH_COREML_FACTORY 1
#endif
#include "picosha2.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>

namespace labyrinth {
namespace {
namespace fs = std::filesystem;
using Team = std::array<int64_t, 5>;

static std::string sha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Missing model file " + path_utf8(path));
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    return picosha2::hash256_hex_string(bytes);
}

struct Random {
    uint64_t state;

    double gumbel() {
        uint64_t z = (state += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        // SplitMix64, with a midpoint in (0, 1) for the two logarithms.
        const auto uniform = (static_cast<double>((z ^ (z >> 31)) >> 12) + 0.5) * 0x1p-52;
        return -std::log(-std::log(uniform));
    }
};

// Kool et al. (2019), Algorithm 1. The conditioned child maximum is the parent priority.
double condition_gumbel(double sampled, double maximum, double parent) {
    if (sampled == maximum) return parent;
    const auto difference = parent - sampled + std::log(-std::expm1(sampled - maximum));
    const auto softplus = std::max(0.0, difference) + std::log1p(std::exp(-std::abs(difference)));
    return parent - softplus;
}
struct Encoded {
    std::vector<float> memory;
    std::vector<uint8_t> valid, owned;
    int64_t length;
};
struct Scored {
    Team team;
    double win = 0, goal = 0, seconds = 0;
};
class Engine final : public TeamModel {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "labyrinth_native"};
    Ort::SessionOptions options;
    Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::unique_ptr<Ort::Session> context, decoder, critic;
    std::unordered_map<int64_t, int64_t> q_index, c_index, unit_index;
    std::unordered_map<Id, int64_t> relic_index;
    std::vector<int64_t> layers;
    std::vector<std::vector<int64_t>> enemies;
    std::vector<double> priors;
    int64_t enemy_count = 5;
    bool critic_relics = false;
    int candidates;
    uint64_t seed;
    std::string backend = "CPU";
    std::vector<int64_t> units;
public:
    explicit Engine(const ModelOptions& settings) : candidates(settings.candidates), seed(settings.candidate_seed) {
        const auto& directory = settings.directory;
        auto manifest = read_json(directory / "manifest.json");
        const auto format = manifest.at("format").get<std::string>();
        const bool use_relics = format == "labyrinth_single_onnx_v2";
        if ((!use_relics && format != "labyrinth_single_onnx_v1") || !manifest.at("files").is_object())
            throw std::runtime_error("Unknown or invalid model manifest");
        const std::set<std::string> required = {"context.onnx", "decoder.onnx", "critic.onnx",
                                               "metadata.json", "export_verification.json"};
        std::set<std::string> names;
        for (auto it = manifest.at("files").begin(); it != manifest.at("files").end(); ++it) {
            if (!required.count(it.key())) throw std::runtime_error("Unexpected model file");
            names.insert(it.key());
            if (sha256(directory / it.key()) != it.value().get<std::string>())
                throw std::runtime_error("Model SHA256 mismatch: " + it.key());
        }
        if (names != required) throw std::runtime_error("Incomplete model bundle");
        auto meta = read_json(directory / "metadata.json");
        const auto critic_format = meta.value("critic_format", std::string("legacy_logits"));
        critic_relics = use_relics && critic_format == "relic_probabilities_v1";
        if (!critic_relics && critic_format != "legacy_logits")
            throw std::runtime_error("Unsupported critic format");
        units = meta.at("unit_ids").get<std::vector<int64_t>>();
        auto quests = meta.at("quest_ids").get<std::vector<int64_t>>();
        if (units.size() != 314 || units.front() <= 0 || units.back() > INT_MAX ||
            !std::is_sorted(units.begin(), units.end()) ||
            std::adjacent_find(units.begin(), units.end()) != units.end())
            throw std::runtime_error("Invalid canonical unit vocabulary");
        if (quests.size() != 254 || std::set<int64_t>(quests.begin(), quests.end()).size() != quests.size() ||
            std::any_of(quests.begin(), quests.end(), [](auto q) { return q <= 0 || q > INT_MAX; }))
            throw std::runtime_error("Invalid quest vocabulary");
        for (size_t i = 0; i < units.size(); ++i) unit_index[units[i]] = static_cast<int64_t>(i);
        for (size_t i = 0; i < quests.size(); ++i) q_index[quests[i]] = static_cast<int64_t>(i + 1);
        if (use_relics) {
            auto relics = meta.at("relic_ids").get<std::vector<int64_t>>();
            if (relics.empty() || relics.size() > 255 || relics.front() <= 0 || relics.back() > INT_MAX ||
                !std::ranges::is_sorted(relics) || std::adjacent_find(relics.begin(), relics.end()) != relics.end())
                throw std::runtime_error("Invalid relic vocabulary");
            for (size_t i = 0; i < relics.size(); ++i)
                relic_index.emplace(static_cast<Id>(relics[i]), static_cast<int64_t>(i + 1));
        }
        auto vocab = meta.at("critic_vocabulary");
        if (vocab.at("unit_ids") != meta.at("unit_ids")) throw std::runtime_error("Critic vocabulary mismatch");
        auto cq = vocab.at("quest_ids").get<std::vector<int64_t>>();
        for (size_t i = 0; i < cq.size(); ++i) c_index[cq[i]] = static_cast<int64_t>(i + 1);
        if (cq.size() != quests.size() || c_index.size() != q_index.size() ||
            std::any_of(cq.begin(), cq.end(), [&](auto q) { return !q_index.count(q); }))
            throw std::runtime_error("Quest vocabulary mismatch");
        layers = meta.at("critic_layers").get<std::vector<int64_t>>();
        enemies = meta.at("critic_enemies").get<std::vector<std::vector<int64_t>>>();
        if (!critic_relics) priors = meta.at("critic_win_prior").get<std::vector<double>>();
        enemy_count = vocab.at("max_enemies").get<int64_t>();
        const auto critic_size = quests.size() + 1;
        if (enemy_count != 5 || layers.size() != critic_size || enemies.size() != critic_size ||
            (!critic_relics && priors.size() != critic_size) ||
            std::any_of(enemies.begin(), enemies.end(), [&](const auto& row) { return row.size() != size_t(enemy_count); }) ||
            std::any_of(priors.begin(), priors.end(), [](double p) { return !std::isfinite(p) || p < 0 || p > 1; }))
            throw std::runtime_error("Invalid critic metadata dimensions");
        if (settings.use_gpu) enable_gpu(settings);
        options.SetIntraOpNumThreads(settings.threads); options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        context = std::make_unique<Ort::Session>(env, (directory / "context.onnx").c_str(), options);
        if (context->GetInputCount() != (use_relics ? 4u : 3u))
            throw std::runtime_error("Context graph input count disagrees with model format");
        if (use_relics) {
            Ort::AllocatorWithDefaultOptions allocator;
            auto name = context->GetInputNameAllocated(3, allocator);
            auto type = context->GetInputTypeInfo(3);
            auto tensor = type.GetTensorTypeAndShapeInfo();
            if (std::string_view(name.get()) != "relic_items" ||
                tensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ||
                tensor.GetShape() != std::vector<int64_t>{1, static_cast<int64_t>(relic_index.size())})
                throw std::runtime_error("Context relic input disagrees with its vocabulary");
        }
        decoder = std::make_unique<Ort::Session>(env, (directory / "decoder.onnx").c_str(), options);
        critic = std::make_unique<Ort::Session>(env, (directory / "critic.onnx").c_str(), options);
        if (critic->GetInputCount() != (critic_relics ? 5u : 4u) || critic->GetOutputCount() != 1)
            throw std::runtime_error("Critic graph inputs or outputs disagree with its format");
        if (critic_relics) {
            Ort::AllocatorWithDefaultOptions allocator;
            auto name = critic->GetInputNameAllocated(4, allocator);
            auto type = critic->GetInputTypeInfo(4);
            auto tensor = type.GetTensorTypeAndShapeInfo();
            auto output_type = critic->GetOutputTypeInfo(0);
            if (std::string_view(name.get()) != "relic_items" ||
                tensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ||
                tensor.GetShape() != std::vector<int64_t>{-1, static_cast<int64_t>(relic_index.size())} ||
                output_type.GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{-1, 6} ||
                std::string_view(critic->GetOutputNameAllocated(0, allocator).get()) != "probabilities")
                throw std::runtime_error("Relic critic graph disagrees with metadata");
        }
    }
    std::string inference_backend() const override { return backend; }

private:
    std::vector<int64_t> encode_relics(const Ids& relics) const {
        std::vector<int64_t> tokens;
        for (Id id : relics) {
            const auto found = relic_index.find(id);
            if (found != relic_index.end()) tokens.push_back(found->second);
        }
        std::ranges::sort(tokens);
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
        tokens.resize(relic_index.size(), 0);
        return tokens;
    }

    void enable_gpu([[maybe_unused]] const ModelOptions& settings) {
        const auto available = Ort::GetAvailableProviders();
#if defined(LABYRINTH_COREML_FACTORY)
        if (std::ranges::find(available, "CoreMLExecutionProvider") == available.end())
            throw std::runtime_error("This ONNX Runtime does not provide CoreML");
        // Dynamic ONNX shapes can fail on CoreML's neural-network converter.
        // Keep dynamic partitions on CPU and use MLProgram for static ones.
        constexpr auto flags = COREML_FLAG_USE_CPU_AND_GPU |
                               COREML_FLAG_ONLY_ALLOW_STATIC_INPUT_SHAPES |
                               COREML_FLAG_CREATE_MLPROGRAM;
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(options, flags));
        backend = "CoreML (CPU + GPU)";
#elif defined(_WIN32)
        if (std::ranges::find(available, "DmlExecutionProvider") == available.end())
            throw std::runtime_error("GPU requires the DirectML build of ONNX Runtime");
        using AppendDirectML = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
        const auto module = GetModuleHandleW(L"onnxruntime.dll");
        const auto append = module ? reinterpret_cast<AppendDirectML>(
            GetProcAddress(module, "OrtSessionOptionsAppendExecutionProvider_DML")) : nullptr;
        if (!append) throw std::runtime_error("Cannot load the ONNX Runtime DirectML provider");
        options.SetExecutionMode(ORT_SEQUENTIAL);
        options.DisableMemPattern();
        const auto adapters = windows_gpu_adapters();
        if (adapters.empty()) throw std::runtime_error("找不到支持 Direct3D 12 的显卡");
        auto chosen = adapters.begin();
        if (settings.gpu_adapter) {
            chosen = std::ranges::find(adapters, *settings.gpu_adapter, &WindowsGpuAdapter::id);
            if (chosen == adapters.end()) throw std::runtime_error("所选显卡不可用，请重新选择 GPU 设备");
        }
        Ort::ThrowOnError(append(options, chosen->index));
        backend = "DirectML · " + chosen->name;
#elif defined(__linux__)
        if (std::ranges::find(available, "CUDAExecutionProvider") == available.end())
            throw std::runtime_error("GPU requires the CUDA build of ONNX Runtime");
        options.AppendExecutionProvider_CUDA(OrtCUDAProviderOptions{});
        backend = "CUDA";
#else
        throw std::runtime_error("No supported GPU execution provider on this platform");
#endif
    }

public:
    bool supports_quest(Id q) const override { return q_index.count(q) != 0; }
    bool supports_unit(Id id) const override { return unit_index.count(id) != 0; }
private:
    std::vector<uint8_t> owned_mask(const Ids& owned) const {
        std::vector<uint8_t> valid(units.size(), 0);
        for (auto id : owned) {
            auto it = unit_index.find(id); if (it != unit_index.end()) valid[it->second] = 1;
        }
        if (std::accumulate(valid.begin(), valid.end(), 0) < 5)
            throw std::runtime_error("Not enough model-supported units");
        return valid;
    }
    Encoded encode(Id quest, int64_t goal, std::vector<uint8_t> valid, const Ids& relics) {
        int64_t qi = q_index.at(quest);
        std::array<int64_t,1> one{1}; std::array<int64_t,2> roster{1, static_cast<int64_t>(units.size())};
        std::vector<Ort::Value> input;
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu, &qi, 1, one.data(), 1));
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu, &goal, 1, one.data(), 1));
        input.emplace_back(Ort::Value::CreateTensor(cpu, valid.data(), valid.size(), roster.data(), 2,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        std::vector<int64_t> relic_tokens;
        if (!relic_index.empty()) {
            relic_tokens = encode_relics(relics);
            const std::array<int64_t, 2> shape{1, static_cast<int64_t>(relic_tokens.size())};
            input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu, relic_tokens.data(), relic_tokens.size(),
                                                               shape.data(), shape.size()));
        }
        const char* names[] = {"quest", "objective_value", "owned_mask", "relic_items"};
        const char* outputs[] = {"memory", "memory_valid"};
        auto output = context->Run(Ort::RunOptions{nullptr}, names, input.data(), input.size(), outputs, 2);
        auto count = output[0].GetTensorTypeAndShapeInfo().GetElementCount();
        auto shape = output[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[0] != 1 || shape[1] < 1 || shape[2] != 256 ||
            output[1].GetTensorTypeAndShapeInfo().GetElementCount() != size_t(shape[1]))
            throw std::runtime_error("Invalid context output");
        auto mem = output[0].GetTensorData<float>(); auto mask = output[1].GetTensorData<bool>();
        return {std::vector<float>(mem, mem + count), std::vector<uint8_t>(mask, mask + shape[1]),
                std::move(valid), shape[1]};
    }
    std::vector<float> decode(Encoded& enc, const std::vector<std::vector<int64_t>>& prefix) {
        int64_t batch = static_cast<int64_t>(prefix.size()), step = static_cast<int64_t>(prefix[0].size());
        std::vector<int64_t> padded(static_cast<size_t>(batch * 4), 0);
        for (int64_t b = 0; b < batch; ++b)
            for (int64_t j = 0; j < step; ++j) padded[b*4+j] = prefix[b][j];
        std::array<int64_t,3> ms{1,enc.length,256};
        std::array<int64_t,2> vs{1,enc.length}, ps{batch,4};
        std::array<int64_t,1> ss{1};
        std::vector<Ort::Value> input;
        input.emplace_back(Ort::Value::CreateTensor<float>(cpu, enc.memory.data(), enc.memory.size(), ms.data(), 3));
        input.emplace_back(Ort::Value::CreateTensor(cpu, enc.valid.data(), enc.valid.size(), vs.data(), 2,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu, padded.data(), padded.size(), ps.data(), 2));
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu, &step, 1, ss.data(), 1));
        const char* names[] = {"memory","memory_valid","prefixes","step"}; const char* outputs[] = {"logits"};
        auto result = decoder->Run(Ort::RunOptions{nullptr}, names, input.data(), 4, outputs, 1);
        auto n = result[0].GetTensorTypeAndShapeInfo().GetElementCount();
        auto values = result[0].GetTensorData<float>();
        if (n != static_cast<size_t>(batch)*units.size()) throw std::runtime_error("Invalid pointer output");
        return std::vector<float>(values, values+n);
    }
    std::vector<Team> enumerate_teams(const std::vector<uint8_t>& mask, size_t combinations) const {
        std::vector<int64_t> roster;
        for (size_t i = 0; i < mask.size(); ++i)
            if (mask[i]) roster.push_back(units[i]);

        // Each permutation of five selected slots describes one ascending team.
        std::vector<uint8_t> selected(roster.size(), 0);
        std::fill_n(selected.begin(), Team{}.size(), 1);
        std::vector<Team> result;
        result.reserve(combinations);
        do {
            Team team{};
            auto member = team.begin();
            for (size_t i = 0; i < roster.size(); ++i)
                if (selected[i]) *member++ = roster[i];
            result.push_back(team);
        } while (std::prev_permutation(selected.begin(), selected.end()));
        return result;
    }

    std::vector<Team> generate_candidates(Id quest, const Ids& owned, const Ids& relics, int seconds,
                                          size_t target, uint64_t seed) {
        if (!supports_quest(quest)) throw std::runtime_error("Unsupported model quest");
        const std::array<int, 4> times{10, 30, 60, 90};
        const auto objective = std::find(times.begin(), times.end(), seconds);
        if (objective == times.end()) throw std::invalid_argument("Unsupported time objective");

        auto mask = owned_mask(owned);
        const auto available = std::accumulate(mask.begin(), mask.end(), uint64_t{0});
        if (available < Team{}.size()) return {};
        uint64_t combinations = 1;
        for (uint64_t k = 1; k <= Team{}.size(); ++k)
            combinations = combinations * (available - k + 1) / k;
        if (combinations < target)
            return enumerate_teams(mask, static_cast<size_t>(combinations));

        auto enc = encode(quest, objective - times.begin(), std::move(mask), relics);
        const size_t vocabulary_size = units.size();
        std::vector<size_t> higher(vocabulary_size);
        size_t count = 0;
        for (size_t i = vocabulary_size; i-- > 0;) {
            higher[i] = count;
            count += enc.owned[i];
        }

        struct Beam {
            std::vector<int64_t> prefix;
            double log_probability = 0.0;
            double priority = 0.0;
        };
        struct Extension {
            size_t parent, unit;
            double log_probability, priority;
        };

        Random random{seed};
        std::vector<Beam> beams(1);
        std::vector<size_t> legal;
        legal.reserve(count);
        constexpr auto team_size = Team{}.size();
        constexpr auto negative_infinity = -std::numeric_limits<double>::infinity();
        for (size_t step = 0; step < team_size; ++step) {
            std::vector<std::vector<int64_t>> prefixes;
            prefixes.reserve(beams.size());
            for (const auto& beam : beams) prefixes.push_back(beam.prefix);
            const auto logits = decode(enc, prefixes);

            std::vector<Extension> extensions;
            extensions.reserve(beams.size() * count);
            for (size_t b = 0; b < beams.size(); ++b) {
                const auto& parent = beams[b];
                const auto* row = logits.data() + b * vocabulary_size;
                const size_t first = parent.prefix.empty() ? 0 : static_cast<size_t>(parent.prefix.back()) + 1;
                const auto slots_left = team_size - step - 1;
                double max_logit = negative_infinity;
                legal.clear();
                for (size_t unit = first; unit < vocabulary_size && higher[unit] >= slots_left; ++unit) {
                    if (!enc.owned[unit]) continue;
                    if (!std::isfinite(row[unit])) throw std::runtime_error("Nonfinite pointer logits");
                    legal.push_back(unit);
                    max_logit = std::max(max_logit, static_cast<double>(row[unit]));
                }
                if (legal.empty()) throw std::runtime_error("No legal pointer completion");

                double sum = 0.0;
                for (auto unit : legal) sum += std::exp(static_cast<double>(row[unit]) - max_logit);
                const auto log_normalizer = std::log(sum);
                const auto first_child = extensions.size();
                double maximum = negative_infinity;
                for (auto unit : legal) {
                    const auto log_probability = parent.log_probability +
                        (static_cast<double>(row[unit]) - max_logit) - log_normalizer;
                    const auto priority = log_probability + random.gumbel();
                    maximum = std::max(maximum, priority);
                    extensions.push_back({b, unit, log_probability, priority});
                }
                for (size_t i = first_child; i < extensions.size(); ++i) {
                    auto& child = extensions[i];
                    child.priority = condition_gumbel(child.priority, maximum, parent.priority);
                }
            }

            const auto kept = std::min(target, extensions.size());
            std::partial_sort(extensions.begin(), extensions.begin() + kept, extensions.end(),
                [](const Extension& a, const Extension& b) {
                    if (a.priority != b.priority) return a.priority > b.priority;
                    if (a.parent != b.parent) return a.parent < b.parent;
                    return a.unit < b.unit;
                });
            std::vector<Beam> next;
            next.reserve(kept);
            for (size_t i = 0; i < kept; ++i) {
                const auto& child = extensions[i];
                auto prefix = beams[child.parent].prefix;
                prefix.push_back(static_cast<int64_t>(child.unit));
                next.push_back({std::move(prefix), child.log_probability, child.priority});
            }
            beams = std::move(next);
        }

        std::vector<Team> result;
        result.reserve(beams.size());
        for (const auto& beam : beams) {
            Team team{};
            std::transform(beam.prefix.begin(), beam.prefix.end(), team.begin(),
                           [&](int64_t unit) { return units[unit]; });
            result.push_back(team);
        }
        return result;
    }
    std::vector<Scored> score(int64_t quest, const std::vector<Team>& teams, const Ids& relics, int seconds) {
        if (teams.empty()) return {};
        int64_t qi=c_index.at(quest), b=static_cast<int64_t>(teams.size());
        std::vector<int64_t> qs(b,qi), ls(b,layers.at(qi)), es, ts;
        for(const auto& team:teams) {
            es.insert(es.end(),enemies.at(qi).begin(),enemies.at(qi).end());
            for(auto unit:team) ts.push_back(unit_index.at(unit)+2);
        }
        std::array<int64_t,1> a{b}; std::array<int64_t,2> e{b,enemy_count}, t{b,5};
        std::vector<Ort::Value> input;
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu,qs.data(),qs.size(),a.data(),1));
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu,ls.data(),ls.size(),a.data(),1));
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu,es.data(),es.size(),e.data(),2));
        input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu,ts.data(),ts.size(),t.data(),2));
        std::vector<int64_t> relic_batch;
        if (critic_relics) {
            const auto items = encode_relics(relics);
            relic_batch.reserve(teams.size() * items.size());
            for (size_t i = 0; i < teams.size(); ++i)
                relic_batch.insert(relic_batch.end(), items.begin(), items.end());
            const std::array<int64_t, 2> shape{b, static_cast<int64_t>(items.size())};
            input.emplace_back(Ort::Value::CreateTensor<int64_t>(cpu, relic_batch.data(), relic_batch.size(),
                                                               shape.data(), shape.size()));
        }
        const char* names[] = {"quests", "layers", "enemies", "teams", "relic_items"};
        const char* outputs[] = {critic_relics ? "probabilities" : "logits"};
        auto output = critic->Run(Ort::RunOptions{nullptr}, names, input.data(), input.size(), outputs, 1);
        const size_t win_bins = critic_relics ? 5 : 4;
        const size_t classes = win_bins + 1;
        if (output[0].GetTensorTypeAndShapeInfo().GetElementCount() != size_t(b) * classes)
            throw std::runtime_error("Invalid critic output");
        const auto* values = output[0].GetTensorData<float>();
        constexpr std::array<int, 5> relic_times{15, 30, 45, 60, 90}, legacy_times{10, 30, 60, 90, 0};
        constexpr std::array<double, 5> relic_mid{7.5, 22.5, 37.5, 52.5, 75}, legacy_mid{5, 20, 45, 75, 0};
        const auto& times = critic_relics ? relic_times : legacy_times;
        const auto& mid = critic_relics ? relic_mid : legacy_mid;
        const double prior = critic_relics ? 0.5 : std::clamp(priors.at(qi), 1e-7, 1. - 1e-7);
        std::vector<Scored> rows;
        for (int64_t i = 0; i < b; ++i) {
            Scored row;
            row.team = teams[i];
            std::array<double, 6> probabilities{};
            for (size_t j = 0; j < classes; ++j) {
                const double value = values[i * classes + j];
                if (!std::isfinite(value) || (critic_relics && (value < 0 || value > 1)))
                    throw std::runtime_error("Invalid critic values");
                probabilities[j] = critic_relics ? value : value + (j < win_bins ? std::log(prior) : std::log1p(-prior));
            }
            if (!critic_relics) {
                const auto end = probabilities.begin() + classes;
                const double maximum = *std::max_element(probabilities.begin(), end);
                double sum = 0;
                for (auto it = probabilities.begin(); it != end; ++it) sum += (*it = std::exp(*it - maximum));
                for (auto it = probabilities.begin(); it != end; ++it) *it /= sum;
            }
            for (size_t j = 0; j < win_bins; ++j) {
                row.win += probabilities[j];
                row.seconds += probabilities[j] * mid[j];
                if (times[j] <= seconds) row.goal += probabilities[j];
            }
            row.win = std::clamp(row.win, 0.0, 1.0);
            row.seconds/=std::max(row.win,1e-12); rows.push_back(row);
        }
        return rows;
    }
public:
    std::vector<RankedParty> recruit_scores(Id quest, const std::vector<Party>& parties, const Ids& relics,
                                            int seconds) override {
        std::vector<Team> teams;
        teams.reserve(parties.size());
        for (const auto& party : parties) {
            if (party.size() != Team{}.size()) throw std::invalid_argument("Invalid recruitment party");
            Team team{};
            std::ranges::copy(party, team.begin());
            teams.push_back(team);
        }
        std::vector<RankedParty> result;
        for (const auto& row : score(quest, teams, relics, seconds))
            result.push_back({Party(row.team.begin(), row.team.end()), row.win});
        return result;
    }

    std::vector<RankedParty> recruit_seeds(Id quest, const Ids& owned, const Ids& relics, int seconds) override {
        constexpr size_t seed_budget = 12;
        auto teams = generate_candidates(quest, owned, relics, seconds, std::min(static_cast<size_t>(candidates), seed_budget),
                                         seed + uint64_t(quest));
        std::vector<RankedParty> result;
        result.reserve(teams.size());
        for (const auto& row : score(quest, teams, relics, seconds))
            result.push_back({Party(row.team.begin(), row.team.end()), row.win});
        return result;
    }

    std::vector<RankedParty> ranked(Id quest, const Ids& owned, const Ids& relics, int seconds, size_t limit) override {
        auto pool = generate_candidates(quest, owned, relics, seconds, candidates, seed + uint64_t(quest));
        if (pool.empty()) throw std::runtime_error("Empty candidate pool");
        auto rows = score(quest, pool, relics, seconds);
        std::sort(rows.begin(), rows.end(), [](const Scored& a, const Scored& b) {
            bool qa = a.win >= .90, qb = b.win >= .90;
            if (qa != qb) return qa;
            if (qa && a.seconds != b.seconds) return a.seconds < b.seconds;
            if (a.win != b.win) return a.win > b.win;
            if (a.goal != b.goal) return a.goal > b.goal;
            if (a.seconds != b.seconds) return a.seconds < b.seconds;
            return a.team < b.team;
        });
        std::vector<RankedParty> result;
        for (size_t i = 0; i < std::min(limit, rows.size()); ++i) {
            const auto& row = rows[i];
            result.push_back({Party(row.team.begin(), row.team.end()), std::clamp(row.win, 0.0, 1.0)});
        }
        return result;
    }
};
} // namespace

std::unique_ptr<TeamModel> load_model(const ModelOptions& options) {
    return std::make_unique<Engine>(options);
}
} // namespace labyrinth
