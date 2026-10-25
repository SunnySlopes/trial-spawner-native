#include "search.hpp"
#include "json.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using Json = nlohmann::json;
using namespace trial;
namespace fs = std::filesystem;

static uint64_t parse_seed(const std::string &text) {
    int64_t number;
    auto begin = text.data();
    if (!text.empty() && text.front() == '+')
        ++begin;
    auto [end, error] = std::from_chars(begin, text.data() + text.size(), number);
    if (error == std::errc{} && end == text.data() + text.size())
        return uint64_t(number);
    if (error == std::errc::result_out_of_range)
        throw std::invalid_argument("种子超出64位整数范围");
    uint32_t hash = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t c = uint8_t(text[i++]);
        if (c >= 0xc0) {
            int n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
            c &= (1U << (6 - n)) - 1;
            while (n-- && i < text.size())
                c = (c << 6) | (uint8_t(text[i++]) & 63);
        }
        if (c > 0xffff) {
            c -= 0x10000;
            hash = hash * 31 + 0xd800 + (c >> 10);
            c = 0xdc00 + (c & 1023);
        }
        hash = hash * 31 + c;
    }
    return uint64_t(int64_t(int32_t(hash)));
}

static int generation_for(const std::string &version) {
    if (version == "3" || version == "26.3")
        return 3;
    if (version == "1" || version == "1.21" || version == "1.21.1")
        return 1;
    constexpr const char *modern[] = {"2",      "1.21.2",  "1.21.3",  "1.21.4", "1.21.5", "1.21.6", "1.21.7", "1.21.8",
                                      "1.21.9", "1.21.10", "1.21.11", "26.1",   "26.1.1", "26.1.2", "26.2"};
    for (auto supported : modern)
        if (version == supported)
            return 2;
    throw std::invalid_argument("支持生成代际1/2/3及Minecraft 1.21–26.3");
}

static void validate(const Options &o) {
    if (o.seed_text.empty())
        throw std::invalid_argument("种子不能为空");
    if (o.radius < 1 || o.radius > 60000000 || o.cluster_radius < 1 || o.cluster_radius > 8192)
        throw std::invalid_argument("搜索范围或统计半径不合法");
    if (o.center_x < -30000000 || o.center_x > 29999999 || o.center_z < -30000000 || o.center_z > 29999999)
        throw std::invalid_argument("搜索中心超出世界边界");
    if (o.threads < 1 || o.threads > 32 || o.min_structures < 1 || o.min_spawners < 0 || o.result_limit < 0)
        throw std::invalid_argument("线程数需为1–32，筛选阈值需非负且密室数至少1");
}

static bool square_shape(const std::string &shape) {
    if (shape == "square")
        return true;
    if (shape == "circle")
        return false;
    throw std::invalid_argument("范围形状需为circle或square");
}

static Json bounds_json(const Options &o) {
    return {{"centerX", o.center_x},
            {"centerZ", o.center_z},
            {"radiusBlocks", o.radius},
            {"shape", o.search_square ? "square" : "circle"},
            {"fullWorld", o.full_world}};
}

static Json params_json(const Options &o) {
    return {{"clusterRadiusBlocks", o.cluster_radius},
            {"clusterShape", o.cluster_square ? "square" : "circle"},
            {"minStructures", o.min_structures},
            {"minSpawners", o.min_spawners},
            {"generation", std::to_string(o.generation)}};
}

// 展示距离统一在这里取整到一位小数，CSV/TXT/JSON 三个出口共用同一数值，避免各自格式化后出现 0.1 的差。
static double distance_of(const Result &r) {
    return std::round(std::sqrt(double(r.distance_sq)) * 10) / 10;
}

static Json candidate_json(const Result &r, int rank) {
    uint64_t h = 1469598103934665603ULL;
    Json structures = Json::array();
    for (auto p : r.structures) {
        h = (h ^ uint32_t(p.x)) * 1099511628211ULL;
        h = (h ^ uint32_t(p.z)) * 1099511628211ULL;
        structures.push_back({p.x, p.z});
    }
    // 现有搜索只优化水平覆盖；这个 Y 是展示高度，不是垂直最优。
    return {{"id", "c-" + std::to_string(h)},
            {"rank", rank},
            {"x", r.x},
            {"z", r.z},
            {"score", r.spawners},
            {"scoreUnit", "刷怪笼"},
            {"distance", distance_of(r)},
            {"distanceSq", r.distance_sq},
            {"secondary", r.count},
            {"secondaryUnit", "密室数"},
            {"bestPlayer", {{"x", r.x}, {"y", -30}, {"z", r.z}}},
            {"tpCommand", "/tp @s " + std::to_string(r.x) + " -30 " + std::to_string(r.z)},
            {"structures", structures}};
}

static Json result_json(const Options &o, const SearchOutput &output, const std::string &job) {
    Json candidates = Json::array();
    int rank = 0;
    for (const auto &r : output.results)
        candidates.push_back(candidate_json(r, ++rank));
    int64_t search_ms = int64_t(std::llround(output.stats.elapsed_ms));
    return {{"jobId", job},
            {"protocolVersion", protocol_version},
            {"rankingRuleVersion", ranking_rule_version},
            {"rankingRule", ranking_rule},
            {"finder", "trial-spawner"},
            {"seed", o.seed_text},
            {"finderGeneration", std::to_string(o.generation)},
            {"engine", engine_name},
            {"engineVersion", engine_version},
            {"bounds", bounds_json(o)},
            {"params", params_json(o)},
            {"threads", o.threads},
            {"device", "cpu"},
            {"requestedLimit", o.result_limit},
            {"returnedCount", int64_t(output.results.size())},
            {"scannedChunks", int64_t(output.stats.chunks)},
            {"elapsedMs", search_ms},
            {"candidates", candidates},
            {"stats",
             {{"candidatePositions", int64_t(output.stats.candidates)},
              {"simulated", int64_t(output.stats.layouts)},
              {"results", int64_t(output.results.size())},
              {"layouts", int64_t(output.stats.layouts)},
              {"prunedClusters", int64_t(output.stats.pruned)},
              {"completedShards", int64_t(output.stats.completed)},
              {"resumedShards", int64_t(output.stats.resumed)}}}};
}

// 三份正式文件来自同一份最终结果：排名只在这里编排一次，CSV/TXT/JSON 的顺序与 rank 完全一致。
static Json write_results(const Options &o, const SearchOutput &output, const fs::path &directory,
                          const std::string &job, double paused_ms, const std::function<double()> &total_ms,
                          Json *exports = nullptr) {
    if (!exports)
        fs::create_directories(directory);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm time{};
#ifdef _WIN32
    localtime_s(&time, &now);
#else
    localtime_r(&now, &time);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&time, "%Y%m%d-%H%M%S");
    fs::path base = directory / ("results-trial-spawner-" + timestamp.str());
    int suffix = 1;
    while (fs::exists(base.string() + ".csv"))
        base = directory / ("results-trial-spawner-" + timestamp.str() + "-" + std::to_string(++suffix));
    std::ostringstream csv, txt;
    csv << "\xef\xbb\xbf排名;距搜索中心距离;中心X;中心Z;密室数量;试炼刷怪笼数量;密室位置\n";
    txt << "排名\t距搜索中心距离\t中心X\t中心Z\t密室数量\t试炼刷怪笼数量\t密室位置\n";
    int rank = 0;
    std::ostringstream distance;
    distance << std::fixed << std::setprecision(1);
    for (const auto &r : output.results) {
        std::ostringstream places;
        bool first = true;
        for (auto p : r.structures) {
            if (!first)
                places << '|';
            places << p.x << ',' << p.z;
            first = false;
        }
        distance.str("");
        distance << std::fixed << std::setprecision(1) << distance_of(r);
        ++rank;
        csv << rank << ';' << distance.str() << ';' << r.x << ';' << r.z << ';' << r.count << ';' << r.spawners << ';'
            << places.str() << '\n';
        txt << rank << '\t' << distance.str() << '\t' << r.x << '\t' << r.z << '\t' << r.count << '\t' << r.spawners
            << '\t' << places.str() << '\n';
    }
    Json result = result_json(o, output, job);
    int64_t search_ms = int64_t(std::llround(output.stats.elapsed_ms));
    result["elapsedMs"] = search_ms;
    result["timing"] = {{"searchMs", search_ms},
                        {"pausedMs", int64_t(std::llround(paused_ms))},
                        {"resumedMs", int64_t(std::llround(output.stats.resumed_ms))},
                        {"totalMs", int64_t(std::llround(total_ms()))}};
    result["output"] = Json::object();
    if (exports) {
        *exports = {{"csv", csv.str()}, {"txt", txt.str()}, {"json", result.dump(2) + "\n"}};
    } else {
        result["output"] = {{"csvPath", fs::absolute(base.string() + ".csv").string()},
                            {"txtPath", fs::absolute(base.string() + ".txt").string()},
                            {"jsonPath", fs::absolute(base.string() + ".json").string()}};
        for (const auto &[ext, content] : std::map<std::string, std::string>{
                 {"csv", csv.str()}, {"txt", txt.str()}, {"json", result.dump(2) + "\n"}}) {
            std::ofstream file(base.string() + "." + ext, std::ios::binary);
            file << content;
            file.close();
            if (!file)
                throw std::runtime_error("写入结果文件失败");
        }
    }
    if (!output.checkpoint_path.empty()) {
        std::error_code error;
        fs::remove(output.checkpoint_path, error);
    }
    return result;
}

static Options from_config(const Json &config) {
    int version = config.value("protocolVersion", 0);
    if (version != protocol_version)
        throw std::invalid_argument("此引擎只接受协议 v3（protocolVersion=" + std::to_string(protocol_version) +
                                    "），请升级桌面端");
    if (config.at("finder") != "trial-spawner")
        throw std::invalid_argument("不支持的查找器");
    Options o;
    o.seed_text = config.at("seed").get<std::string>();
    o.seed = parse_seed(o.seed_text);
    const auto &bounds = config.at("bounds");
    o.center_x = bounds.value("centerX", 0);
    o.center_z = bounds.value("centerZ", 0);
    o.radius = bounds.at("radiusBlocks");
    o.search_square = square_shape(bounds.at("shape").get<std::string>());
    o.full_world = bounds.value("fullWorld", false);
    o.threads = config.value("threads", 8);
    o.result_limit = config.value("resultLimit", 0);
    if (o.result_limit < 1 || o.result_limit > 200)
        throw std::invalid_argument("结果数量需为1–200");
    const auto &params = config.at("params");
    o.generation = generation_for(params.value("generation", std::string("2")));
    o.cluster_radius = params.value("clusterRadiusBlocks", 128);
    o.min_structures = params.value("minStructures", 2);
    o.min_spawners = params.value("minSpawners", 20);
    // v3 用 clusterShape 表达统计形状；旧配置里的 mode 已无执行语义，直接忽略。
    o.cluster_square = square_shape(params.value("clusterShape", std::string("square")));
    o.checkpoint_directory = "config/checkpoints/trial-native";
    validate(o);
    return o;
}

static Json progress_json(const Progress &p, const std::string &phase, const std::string &state) {
    double percentage = p.total ? 100.0 * double(p.completed) / double(p.total) : 0.0;
    if (phase == "done")
        percentage = 100;
    double speed = p.total && p.elapsed_ms > 0 ? double(p.completed) * 1000.0 / p.elapsed_ms : 0;
    double eta = p.completed ? p.elapsed_ms * (p.total - p.completed) / double(p.completed) : 0;
    // 分片完成数是唯一可靠的总体口径：overallPercentage 只按它算，不给未知阶段编造百分比。
    Json event = {{"phase", phase},
                  {"current", int64_t(p.completed)},
                  {"total", int64_t(p.total)},
                  {"unit", "分片"},
                  {"percentage", std::round(percentage * 10) / 10},
                  {"speedPerSecond", speed},
                  {"elapsedMs", int64_t(p.elapsed_ms)},
                  {"etaMs", int64_t(eta)},
                  {"foundCandidates", int64_t(p.found)},
                  {"state", state},
                  {"message", phase == "done"         ? "搜索完成"
                              : state == "paused"     ? "已暂停"
                              : state == "cancelling" ? "正在中止"
                                                      : "原生布局计算与聚类精排"}};
    if (p.total > 0)
        event["overallPercentage"] = std::round(percentage * 10) / 10;
    return event;
}

static int protocol() {
    std::mutex output_mutex, stats_mutex;
    std::thread worker;
    std::shared_ptr<Control> control;
    Progress stats;
    std::string job;
    Json last_result;
    std::mutex result_mutex;
    uint64_t sequence = 0;
    std::atomic<bool> active{};
    auto emit = [&](const Json &event) {
        std::lock_guard lock(output_mutex);
        std::cout << event.dump() << '\n' << std::flush;
    };
    for (std::string line; std::getline(std::cin, line);) {
        try {
            Json command = Json::parse(line);
            std::string type = command.at("type");
            if (type == "start") {
                if (worker.joinable()) {
                    control->cancel();
                    worker.join();
                }
                job = "native-" + std::to_string(++sequence);
                stats = {};
                control = std::make_shared<Control>();
                emit({{"type", "accepted"}, {"requestId", command.at("requestId")}, {"jobId", job}});
                active = false;
                Options options = from_config(command.at("config"));
                std::string current_job = job;
                auto current_control = control;
                active = true;
                bool defer_export = command.at("config").value("deferExport", false);
                worker = std::thread([&, options, current_job, current_control, defer_export] {
                    // totalMs 含暂停与断点恢复；延迟导出时只计到结果准备完成。
                    auto task_start = std::chrono::steady_clock::now();
                    try {
                        emit({{"type", "progress"},
                              {"jobId", current_job},
                              {"progress", progress_json({}, "prepare", "running")}});
                        auto last_progress = std::chrono::steady_clock::now();
                        auto output = search(options, *current_control, [&](const Progress &p) {
                            {
                                std::lock_guard lock(stats_mutex);
                                stats = p;
                            }
                            std::lock_guard lock(output_mutex);
                            auto now = std::chrono::steady_clock::now();
                            if (current_control->running() && now - last_progress >= std::chrono::milliseconds(70)) {
                                std::cout << Json({{"type", "progress"},
                                                   {"jobId", current_job},
                                                   {"progress", progress_json(p, "coarse", "running")}})
                                                 .dump()
                                          << '\n'
                                          << std::flush;
                                last_progress = now;
                            }
                        });
                        current_control->checkpoint();
                        // totalMs 加上断点恢复的历史累计，
                        // 保证它与同样含历史累计的 searchMs 可比；不把 CPU 核心时间相加。
                        auto total = [&] {
                            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                             task_start)
                                       .count() +
                                   output.stats.resumed_ms;
                        };
                        Json exports;
                        auto result =
                            write_results(options, output, "results", current_job, current_control->paused_ms(), total,
                                          defer_export ? &exports : nullptr);
                        auto &candidates = result["candidates"];
                        for (size_t first = 0; first < candidates.size(); first += 100) {
                            if (first)
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            current_control->checkpoint();
                            Json batch = Json::array();
                            for (size_t i = first; i < std::min(first + 100, candidates.size()); ++i)
                                batch.push_back(candidates[i]);
                            emit({{"type", "candidates"},
                                  {"jobId", current_job},
                                  {"batch", batch},
                                  {"totalFound", candidates.size()}});
                        }
                        {
                            std::lock_guard lock(result_mutex);
                            last_result = result;
                        }
                        active = false;
                        emit({{"type", "progress"},
                              {"jobId", current_job},
                              {"progress", progress_json(output.stats, "done", "done")}});
                        Json done = {{"type", "done"}, {"jobId", current_job}, {"result", result}};
                        if (defer_export)
                            done["exports"] = std::move(exports);
                        emit(done);
                    } catch (const Cancelled &) {
                        active = false;
                        emit({{"type", "failed"},
                              {"jobId", current_job},
                              {"code", "cancelled"},
                              {"message", "搜索已中止"}});
                    } catch (const std::bad_alloc &) {
                        active = false;
                        emit({{"type", "failed"},
                              {"jobId", current_job},
                              {"code", "out-of-memory"},
                              {"message", "内存不足，请减少线程数或统计半径"}});
                    } catch (const std::exception &e) {
                        active = false;
                        emit({{"type", "failed"}, {"jobId", current_job}, {"code", "io-error"}, {"message", e.what()}});
                    }
                });
            } else if (control && active && (type == "pause" || type == "resume" || type == "cancel")) {
                if (command.value("jobId", std::string{}) != job) {
                    std::cerr << "忽略旧任务命令：" << command.dump() << '\n';
                    continue;
                }
                if (type == "pause")
                    control->pause();
                else if (type == "resume")
                    control->resume();
                else
                    control->cancel();
                Progress current;
                {
                    std::lock_guard lock(stats_mutex);
                    current = stats;
                }
                emit({{"type", "progress"},
                      {"jobId", job},
                      {"progress", progress_json(current, "coarse",
                                                 type == "pause"    ? "paused"
                                                 : type == "resume" ? "running"
                                                                    : "cancelling")}});
            } else if (type == "export") {
                std::lock_guard lock(result_mutex);
                if (!last_result.is_null())
                    std::cerr << "结果已保存：" << last_result["output"].dump() << '\n';
            }
        } catch (const std::exception &e) {
            emit({{"type", "failed"}, {"jobId", job}, {"code", "invalid-config"}, {"message", e.what()}});
        }
    }
    if (worker.joinable()) {
        control->cancel();
        worker.join();
    }
    return 0;
}

static int run(int argc, char **argv) {
    try {
        std::map<std::string, std::string> args;
        for (int i = 1; i < argc; ++i) {
            std::string name = argv[i];
            if (name == "--jsonl")
                return protocol();
            if (name == "--help") {
                std::cout << "trial-spawner --seed <seed> --version <1.21.1|26.2|26.3> --radius <blocks>\n"
                             "  --cluster-radius 128 --shape circle --cluster-shape circle\n"
                             "  --min-structures 2 --min-spawners 20 --threads 8 --limit 0\n"
                             "  --center-x 0 --center-z 0 --output results --full-world\n"
                             "  --chunk-x <chunk> --chunk-z <chunk>  (single chamber JSON)\n"
                             "  --config finder.properties --checkpoint-dir results/checkpoints\n"
                             "  --no-checkpoint --jsonl  (桌面协议 v3，stdin/stdout)\n";
                return 0;
            }
            const std::vector<std::string> names = {
                "--seed",           "--version",        "--generation",   "--radius",  "--cluster-radius", "--shape",
                "--cluster-shape",  "--min-structures", "--min-spawners", "--threads", "--limit",          "--center-x",
                "--center-z",       "--output",         "--full-world",   "--chunk-x", "--chunk-z",        "--config",
                "--checkpoint-dir", "--no-checkpoint",  "--finder"};
            if (std::find(names.begin(), names.end(), name) == names.end())
                throw std::invalid_argument("未知参数：" + name);
            if (name == "--full-world" || name == "--no-checkpoint")
                args[name] = "true";
            else {
                if (i + 1 == argc)
                    throw std::invalid_argument("缺少参数值：" + name);
                args[name] = argv[++i];
            }
        }
        if (args.contains("--config")) {
            std::ifstream file(args.at("--config"));
            if (!file)
                throw std::invalid_argument("无法读取配置文件");
            const std::map<std::string, std::string> keys = {{"seed", "--seed"},
                                                             {"minecraft-version", "--version"},
                                                             {"search-center-x", "--center-x"},
                                                             {"search-center-z", "--center-z"},
                                                             {"search-radius-blocks", "--radius"},
                                                             {"search-area-shape", "--shape"},
                                                             {"scan-threads", "--threads"},
                                                             {"trial-cluster-radius-blocks", "--cluster-radius"},
                                                             {"trial-area-shape", "--cluster-shape"},
                                                             {"trial-min-structures", "--min-structures"},
                                                             {"trial-min-spawners", "--min-spawners"},
                                                             {"cluster-radius-blocks", "--cluster-radius"},
                                                             {"area-shape", "--cluster-shape"},
                                                             {"min-structures", "--min-structures"},
                                                             {"min-spawners", "--min-spawners"}};
            for (std::string line; std::getline(file, line);) {
                auto first = line.find_first_not_of(" \t\r\xef\xbb\xbf");
                if (first == std::string::npos || line[first] == '#')
                    continue;
                auto equal = line.find('=', first);
                if (equal == std::string::npos)
                    continue;
                auto name = line.substr(first, equal - first);
                name.erase(name.find_last_not_of(" \t\r") + 1);
                auto value = line.substr(equal + 1);
                auto begin = value.find_first_not_of(" \t\r");
                value = begin == std::string::npos ? "" : value.substr(begin);
                value.erase(value.find_last_not_of(" \t\r") + 1);
                auto mapped = keys.find(name);
                if (mapped != keys.end() && !args.contains(mapped->second))
                    args[mapped->second] = value;
                if (name == "full-world" && value == "true")
                    args["--full-world"] = "true";
            }
        }
        if (args.contains("--finder") && args.at("--finder") != "trial-spawner")
            throw std::invalid_argument("此引擎只支持trial-spawner");
        Options o;
        auto text = [&](const std::string &name, std::string value) {
            auto it = args.find(name);
            return it == args.end() ? value : it->second;
        };
        auto integer = [&](const std::string &name, int value) {
            auto input = text(name, std::to_string(value));
            int number;
            auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), number);
            if (error != std::errc{} || end != input.data() + input.size())
                throw std::invalid_argument("参数需为整数：" + name);
            return number;
        };
        o.seed_text = text("--seed", "0");
        o.seed = parse_seed(o.seed_text);
        o.generation = generation_for(text("--version", text("--generation", "2")));
        o.center_x = integer("--center-x", 0);
        o.center_z = integer("--center-z", 0);
        o.radius = integer("--radius", 100000);
        o.cluster_radius = integer("--cluster-radius", 128);
        o.min_structures = integer("--min-structures", 2);
        o.min_spawners = integer("--min-spawners", 20);
        o.threads = integer("--threads", 8);
        o.result_limit = integer("--limit", 0);
        o.search_square = square_shape(text("--shape", "circle"));
        o.cluster_square = square_shape(text("--cluster-shape", "circle"));
        o.full_world = args.contains("--full-world");
        validate(o);
        if (!args.contains("--no-checkpoint"))
            o.checkpoint_directory =
                text("--checkpoint-dir", (fs::path(text("--output", "results")) / "checkpoints").string());
        if (args.contains("--chunk-x") || args.contains("--chunk-z")) {
            LayoutEngine engine(o.generation, o.seed);
            int x = integer("--chunk-x", 0), z = integer("--chunk-z", 0);
            auto chamber = engine.generate(x, z);
            Json out = {{"seed", o.seed_text},
                        {"generation", o.generation},
                        {"chunkX", x},
                        {"chunkZ", z},
                        {"exists", chamber.exists},
                        {"pieces", chamber.pieces},
                        {"spawners", Json::array()}};
            for (auto p : chamber.spawners)
                out["spawners"].push_back({p.x, p.y, p.z});
            std::cout << out.dump() << '\n';
            return 0;
        }
        Control control;
        auto cli_start = std::chrono::steady_clock::now();
        auto output = search(o, control, [](const Progress &p) {
            std::cerr << "分片 " << p.completed << '/' << p.total << "，候选 " << p.candidates << "，布局 " << p.layouts
                      << "，裁剪 " << p.pruned << '\n';
        });
        auto total = [&] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cli_start).count();
        };
        std::cout << write_results(o, output, text("--output", "results"), "cli", control.paused_ms(), total).dump()
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> utf8;
    utf8.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        int length = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(length, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), length, nullptr, nullptr);
        value.pop_back();
        utf8.push_back(std::move(value));
    }
    std::vector<char *> arguments;
    for (auto &value : utf8)
        arguments.push_back(value.data());
    return run(argc, arguments.data());
}
#else
int main(int argc, char **argv) {
    return run(argc, argv);
}
#endif
