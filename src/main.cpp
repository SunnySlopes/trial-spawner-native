#include "search.hpp"
#include "json.hpp"
#include "range_units.hpp"
#include "cli_config.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
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
    return generation_for_version(version);
}

static int midpoint(int a, int b) {
    return int((int64_t(a) + int64_t(b)) / 2);
}

static void validate(const Options &o) {
    if (o.seed_text.empty())
        throw std::invalid_argument("种子不能为空");
    if (o.cluster_rect) {
        if (o.cluster_long < 1 || o.cluster_long > 16384 || o.cluster_short < 1 || o.cluster_short > 16384)
            throw std::invalid_argument("统计窗口长边/短边需为1–16384");
    } else if (o.cluster_radius < 1 || o.cluster_radius > 8192) {
        throw std::invalid_argument("搜索范围或统计半径不合法");
    }
    if (!o.full_world) {
        if (o.search_rect) {
            if (o.min_x > o.max_x || o.min_z > o.max_z)
                throw std::invalid_argument("搜索四至需满足 minX<=maxX 且 minZ<=maxZ");
            if (o.min_x < -30000000 || o.max_x > 29999999 || o.min_z < -30000000 || o.max_z > 29999999)
                throw std::invalid_argument("搜索四至超出世界边界");
        } else {
            if (o.radius < 1 || o.radius > 60000000)
                throw std::invalid_argument("搜索范围或统计半径不合法");
            if (o.center_x < -30000000 || o.center_x > 29999999 || o.center_z < -30000000 ||
                o.center_z > 29999999)
                throw std::invalid_argument("搜索中心超出世界边界");
        }
    }
    if (o.threads < 1 || o.threads > 32 || o.min_structures < 1 || o.min_spawners < 0 || o.result_limit < 0)
        throw std::invalid_argument("线程数需为1–32，筛选阈值需非负且密室数至少1");
    if (o.max_structures < 0 || (o.max_structures > 0 && o.min_structures > o.max_structures))
        throw std::invalid_argument("联数上限需为非负整数，且不能小于最低密室数");
    if (!(o.conversion_ratio >= 0.0 && o.conversion_ratio <= 1.0))
        throw std::invalid_argument("慢速笼折算比例需在0–1");
    if (o.strict_margin < 0)
        throw std::invalid_argument("严格模式边距需为非负整数");
    if (o.strict_mode) {
        // 结构起点要能从四边各缩进 strict_margin 格，窗口必须比它宽；否则内窗口为空。
        const int half_x = o.cluster_rect ? o.cluster_long / 2 : o.cluster_radius;
        const int half_z = o.cluster_rect ? o.cluster_short / 2 : o.cluster_radius;
        if (half_x <= o.strict_margin || half_z <= o.strict_margin)
            throw std::invalid_argument("严格模式要求统计窗口半宽大于边距（长/短边>2×边距，或半径>边距）");
    }
}

static bool square_shape(const std::string &shape) {
    if (shape == "square")
        return true;
    if (shape == "circle")
        return false;
    throw std::invalid_argument("范围形状需为circle或square");
}

static Json bounds_json(const Options &o) {
    const bool rect = o.search_rect;
    Json out = {{"centerX", rect ? midpoint(o.min_x, o.max_x) : o.center_x},
                {"centerZ", rect ? midpoint(o.min_z, o.max_z) : o.center_z},
                {"radiusBlocks", o.radius},
                {"shape", rect ? "rect" : (o.search_square ? "square" : "circle")},
                {"fullWorld", o.full_world}};
    if (rect) {
        out["minX"] = o.min_x;
        out["maxX"] = o.max_x;
        out["minZ"] = o.min_z;
        out["maxZ"] = o.max_z;
    }
    return out;
}

static Json params_json(const Options &o) {
    Json out = {{"clusterRadiusBlocks", o.cluster_radius},
                {"clusterShape", o.cluster_rect ? "rect" : (o.cluster_square ? "square" : "circle")},
                {"minStructures", o.min_structures},
                {"minSpawners", o.min_spawners},
                {"generation", std::to_string(o.generation)},
                {"conversionRatio", o.conversion_ratio},
                {"strictMode", o.strict_mode}};
    if (o.strict_mode)
        out["strictMargin"] = o.strict_margin;
    if (o.max_structures > 0)
        out["maxStructures"] = o.max_structures;
    if (o.cluster_rect) {
        out["clusterLongSide"] = o.cluster_long;
        out["clusterShortSide"] = o.cluster_short;
    }
    return out;
}

// 展示距离统一在这里取整到一位小数，CSV/TXT/JSON 三个出口共用同一数值，避免各自格式化后出现 0.1 的差。
static double distance_of(const Result &r) {
    return std::round(std::sqrt(double(r.distance_sq)) * 10) / 10;
}

static Json candidate_json(const Result &r, int rank, double conversion_ratio) {
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
            {"fastSpawnerCount", r.fast_spawners},
            {"slowSpawnerCount", r.slow_spawners},
            {"convertedSpawnerCount", converted_spawners(r, conversion_ratio)},
            {"conversionRatio", conversion_ratio},
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
        candidates.push_back(candidate_json(r, ++rank, o.conversion_ratio));
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
            {"cubiomesCommit", CUBIOMES_REVISION},
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
    auto named = [&](const std::string &ext) { auto path = base; path += ext; return path; };
    auto pathText = [](const fs::path &path) { auto text = path.u8string(); return std::string(text.begin(), text.end()); };
    int suffix = 1;
    while (fs::exists(named(".csv")))
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
        result["output"] = {{"csvPath", pathText(fs::absolute(named(".csv")))},
                            {"txtPath", pathText(fs::absolute(named(".txt")))},
                            {"jsonPath", pathText(fs::absolute(named(".json")))}};
        for (const auto &[ext, content] : std::map<std::string, std::string>{
                 {"csv", csv.str()}, {"txt", txt.str()}, {"json", result.dump(2) + "\n"}}) {
            std::ofstream file(named("." + ext), std::ios::binary);
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
    o.radius = range_units::field(bounds, "radius", "radiusBlocks", 100000);
    o.full_world = bounds.value("fullWorld", false);
    // 新协议可选携带精确四至；四个键必须同时出现。shape=rect 只在有四至时出现。
    const bool has_rect = bounds.contains("minX") || bounds.contains("maxX") || bounds.contains("minZ") ||
                          bounds.contains("maxZ");
    if (has_rect) {
        if (!bounds.contains("minX") || !bounds.contains("maxX") || !bounds.contains("minZ") ||
            !bounds.contains("maxZ"))
            throw std::invalid_argument("精确四至需同时提供 minX/maxX/minZ/maxZ");
        o.search_rect = true;
        o.min_x = bounds.at("minX");
        o.max_x = bounds.at("maxX");
        o.min_z = bounds.at("minZ");
        o.max_z = bounds.at("maxZ");
        o.center_x = midpoint(o.min_x, o.max_x);
        o.center_z = midpoint(o.min_z, o.max_z);
        o.radius = std::max(o.max_x - o.min_x, o.max_z - o.min_z) / 2 + 1;
        o.search_square = true;
    } else {
        o.search_square = square_shape(bounds.value("shape", std::string("circle")));
    }
    o.threads = config.value("threads", 8);
    o.result_limit = config.value("resultLimit", 0);
    if (o.result_limit < 1 || o.result_limit > 200)
        throw std::invalid_argument("结果数量需为1–200");
    const auto &params = config.at("params");
    o.generation = generation_for(params.value("generation", std::string("2")));
    o.cluster_radius = range_units::field(params, "clusterRadius", "clusterRadiusBlocks", 128);
    const bool has_window = params.contains("clusterLongSide") || params.contains("clusterShortSide");
    if (has_window) {
        if (!params.contains("clusterLongSide") || !params.contains("clusterShortSide"))
            throw std::invalid_argument("矩形统计窗口需同时提供 clusterLongSide/clusterShortSide");
        o.cluster_rect = true;
        o.cluster_long = params.at("clusterLongSide");
        o.cluster_short = params.at("clusterShortSide");
        o.cluster_radius = std::max(o.cluster_long, o.cluster_short) / 2;
        o.cluster_square = false;
    } else {
        // v3 用 clusterShape 表达统计形状；旧配置里的 mode 已无执行语义，直接忽略。
        o.cluster_square = square_shape(params.value("clusterShape", std::string("square")));
    }
    o.min_structures = params.value("minStructures", 2);
    o.min_spawners = params.value("minSpawners", 20);
    o.max_structures = params.value("maxStructures", 0);
    o.conversion_ratio = params.value("conversionRatio", 1.0);
    o.strict_mode = params.value("strictMode", false);
    o.strict_margin = params.value("strictMargin", default_strict_margin);
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
    // 已扫描区块按完成分片比例折算：分片是唯一可靠的进度口径，区块数只是给界面看的量纲。
    if (p.total > 0 && p.chunks > 0)
        event["scannedChunks"] = int64_t(p.chunks * p.completed / p.total);
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
                        // 节流会吞掉最后一帧，这里补发一次粗筛末帧，保证阶段能显示到 100%。
                        emit({{"type", "progress"},
                              {"jobId", current_job},
                              {"progress", progress_json(output.stats, "coarse", "running")}});
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
        /* ⚠️ 不要在这里 cancel。
         *
         * 曾经写成「stdin 一关闭就 cancel()」，结果单行 start 输入（stdin 立即 EOF）
         * 的任务刚跑完就被判成 cancelled，done 帧缺失、桌面端把它当失败。
         * 桌面端在任务结束前一直持有 stdin；即使提前 EOF 也应等任务**自然跑完**。
         * 取消是显式命令，不能由 EOF 隐式触发。
         * 女巫小屋与河流引擎的协议里也踩过同一个坑，修法一致。 */
        worker.join();
    }
    return 0;
}

static int run(int argc, char **argv) {
    try {
        std::vector<std::string> input(argv + 1, argv + argc);
        for (const auto &name : input)
            if (name == "--desktop" || name == "--jsonl")
                return protocol();
        auto command = cli_config::arguments(input);
        std::map<std::string, std::string> args;
        for (size_t i = 0; i < command.size(); ++i) {
            const auto &name = command[i];
            if (name == "--jsonl")
                return protocol();
            if (name == "--help") {
                std::cout << "试炼刷怪笼独立引擎\n"
                             "用法: trial-spawner --seed 种子 [参数]\n"
                             "通用: --config finder.json --version 26.3 --radius 100000 --shape circle|square\n"
                             "      --center-x 0 --center-z 0 --threads 8 --limit 100 --output results\n"
                             "      --radius-unit blocks|chunks --full-world --no-checkpoint\n"
                             "      --min-x/--max-x/--min-z/--max-z 精确四至（四个同时给出，覆盖中心与半径）\n"
                             "统计: --cluster-radius 128 --cluster-radius-unit blocks|chunks --cluster-shape circle|square\n"
                             "      --cluster-long/--cluster-short 矩形统计窗口（同时给出，覆盖统计半径）\n"
                             "      --min-structures 2 --min-spawners 20（窗口内最低等效笼数，非快慢总数）\n"
                             "      --conversion-ratio 1.0 慢速笼折算比例；等效笼数=fast+slow*ratio，即排名键\n"
                             "      --strict-mode --strict-margin 0 严格模式内缩格数；笼数仍按完整窗口统计\n"
                             "      --max-structures 0 每组参与统计的密室数上限，0=不限制\n"
                             "桌面: --desktop (协议v3，stdin/stdout JSONL，不读取CLI配置)\n"
                             "开发: --chunk-x 区块X --chunk-z 区块Z；--limit 0保留旧分组榜单\n"
                             "配置键为CLI参数名去掉--，命令行覆盖配置；输出目录由--output指定。\n";
                return 0;
            }
            const std::vector<std::string> names = {
                "--radius-unit",  "--cluster-radius-unit", "--seed",           "--version",
                "--generation",   "--radius",              "--min-x",          "--max-x",
                "--min-z",        "--max-z",               "--shape",          "--cluster-radius",
                "--cluster-long", "--cluster-short",       "--cluster-shape",  "--min-structures",
                "--min-spawners", "--threads",             "--limit",          "--conversion-ratio",
                "--center-z",       "--output",         "--full-world",   "--chunk-x", "--chunk-z",        "--config",
                "--checkpoint-dir", "--no-checkpoint",  "--strict-mode",  "--strict-margin", "--max-structures", "--finder"};
            if (std::find(names.begin(), names.end(), name) == names.end())
                throw std::invalid_argument("未知参数：" + name);
            if (name == "--full-world" || name == "--no-checkpoint" || name == "--strict-mode")
                args[name] = "true";
            else {
                if (i + 1 == command.size())
                    throw std::invalid_argument("缺少参数值：" + name);
                args[name] = command[++i];
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
        auto real = [&](const std::string &name, double value) {
            auto input = text(name, std::to_string(value));
            char *end = nullptr;
            errno = 0;
            const double number = std::strtod(input.c_str(), &end);
            if (errno == ERANGE || !end || *end != '\0')
                throw std::invalid_argument("参数需为数字：" + name);
            return number;
        };
        o.seed_text = text("--seed", "0");
        o.seed = parse_seed(o.seed_text);
        o.generation = generation_for(text("--version", text("--generation", "2")));
        const bool has_rect = args.contains("--min-x") || args.contains("--max-x") || args.contains("--min-z") ||
                              args.contains("--max-z");
        if (has_rect) {
            if (!args.contains("--min-x") || !args.contains("--max-x") || !args.contains("--min-z") ||
                !args.contains("--max-z"))
                throw std::invalid_argument("精确四至需同时提供 --min-x/--max-x/--min-z/--max-z");
            o.search_rect = true;
            o.min_x = integer("--min-x", 0);
            o.max_x = integer("--max-x", 0);
            o.min_z = integer("--min-z", 0);
            o.max_z = integer("--max-z", 0);
            o.center_x = midpoint(o.min_x, o.max_x);
            o.center_z = midpoint(o.min_z, o.max_z);
            o.radius = std::max(o.max_x - o.min_x, o.max_z - o.min_z) / 2 + 1;
            o.search_square = true;
        } else {
            o.center_x = integer("--center-x", 0);
            o.center_z = integer("--center-z", 0);
            o.radius = range_units::text(text("--radius", "100000"), text("--radius-unit", "blocks"));
            o.search_square = square_shape(text("--shape", "circle"));
        }
        const bool has_window = args.contains("--cluster-long") || args.contains("--cluster-short");
        if (has_window) {
            if (!args.contains("--cluster-long") || !args.contains("--cluster-short"))
                throw std::invalid_argument("矩形统计窗口需同时提供 --cluster-long/--cluster-short");
            o.cluster_rect = true;
            o.cluster_long = range_units::text(text("--cluster-long", "320"), text("--cluster-radius-unit", "blocks"));
            o.cluster_short = range_units::text(text("--cluster-short", "320"), text("--cluster-radius-unit", "blocks"));
            o.cluster_radius = std::max(o.cluster_long, o.cluster_short) / 2;
            o.cluster_square = false;
        } else {
            o.cluster_radius =
                range_units::text(text("--cluster-radius", "128"), text("--cluster-radius-unit", "blocks"));
            o.cluster_square = square_shape(text("--cluster-shape", "circle"));
        }
        o.min_structures = integer("--min-structures", 2);
        o.min_spawners = integer("--min-spawners", 20);
        o.max_structures = integer("--max-structures", 0);
        o.threads = integer("--threads", 8);
        o.result_limit = integer("--limit", 100);
        o.conversion_ratio = real("--conversion-ratio", 1.0);
        o.strict_mode = text("--strict-mode", "false") == "true";
        o.strict_margin = integer("--strict-margin", default_strict_margin);
        o.full_world = args.contains("--full-world");
        validate(o);
        if (!args.contains("--no-checkpoint"))
            o.checkpoint_directory =
                text("--checkpoint-dir", (fs::path(text("--output", "results")) / "checkpoints").string());
        if (args.contains("--chunk-x") || args.contains("--chunk-z")) {
            LayoutEngine engine(o.generation, o.seed);
            int x = integer("--chunk-x", 0), z = integer("--chunk-z", 0);
            std::vector<PieceBox> boxes;
            auto chamber = engine.generate(x, z, &boxes);
            Json out = {{"seed", o.seed_text},
                        {"generation", o.generation},
                        {"chunkX", x},
                        {"chunkZ", z},
                        {"exists", chamber.exists},
                        {"pieces", chamber.pieces},
                        {"spawners", Json::array()},
                        {"slowSpawners", Json::array()},
                        {"boxes", Json::array()}};
            for (auto p : chamber.spawners)
                out["spawners"].push_back({p.x, p.y, p.z});
            for (auto p : chamber.slow_spawners)
                out["slowSpawners"].push_back({p.x, p.y, p.z});
            for (auto &b : boxes)
                out["boxes"].push_back({{"minX", b.box.lo.x},
                                        {"minY", b.box.lo.y},
                                        {"minZ", b.box.lo.z},
                                        {"maxX", b.box.hi.x},
                                        {"maxY", b.box.hi.y},
                                        {"maxZ", b.box.hi.z},
                                        {"slow", b.slow},
                                        {"parent", b.parent},
                                        {"spawners", b.spawners},
                                        {"slowSpawners", b.slow_spawners}});
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
        auto directory = text("--output", "results");
        auto path = fs::path(std::u8string(directory.begin(), directory.end()));
        std::cout << write_results(o, output, path, "cli", control.paused_ms(), total).dump()
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
