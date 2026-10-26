#pragma once
#include "layout.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#ifndef TRIAL_ENGINE_VERSION
#define TRIAL_ENGINE_VERSION "3.0.2"
#endif

namespace trial {
// 桌面契约 v3：正式排名「折算数量降序 → 距搜索中心水平距离平方升序 → X 升序 → Z 升序」。
// conversion_ratio = 1.0 时折算数量等于总刷怪笼数，保持旧榜单。
inline constexpr int protocol_version = 3;
inline constexpr int ranking_rule_version = 4;
inline constexpr const char *ranking_rule = "converted-desc-distance-asc-x-asc-z-asc";
inline constexpr const char *engine_name = "trial-spawner-native";
inline constexpr const char *engine_version = TRIAL_ENGINE_VERSION;

// 世界边界内的极端距离平方仍小于 2^53，桌面可直接当精确整数读；比较全程整数，不用浮点。
inline int64_t squared_distance(int x, int z, int center_x, int center_z) {
    int64_t dx = int64_t(x) - center_x, dz = int64_t(z) - center_z;
    return dx * dx + dz * dz;
}

// 每种密室数量的榜单容量：请求的 Top K 更大时不能被旧的 100 条上限提前裁掉。
inline int ranking_capacity(int result_limit) {
    return result_limit > 0 ? std::max(100, result_limit) : 100;
}

struct Options {
    uint64_t seed{};
    std::string seed_text = "0";
    int generation = 2, center_x = 0, center_z = 0, radius = 100000, cluster_radius = 128;
    int min_structures = 2, min_spawners = 20, threads = 8, result_limit = 0;
    int shard_size = 0;
    // 快速笼按 1 计，慢速笼按该比例折算；1.0 = 旧的总笼数排名。
    double conversion_ratio = 1.0;
    bool search_square = false, cluster_square = false, full_world = false;
    std::string checkpoint_directory;
};

struct Result {
    int x{}, z{}, count{}, spawners{};
    std::vector<Point> structures;
    int64_t distance_sq{};
    int fast_spawners{};
    int slow_spawners{};
};

// 旧测试/旧调用可能只填 spawners；快慢字段未填时退回总笼数，保证 ratio=1.0 兼容。
inline double converted_spawners(const Result &r, double ratio) {
    const int split = r.fast_spawners + r.slow_spawners;
    if (split == 0 && r.spawners > 0) return double(r.spawners);
    return double(r.fast_spawners) + double(r.slow_spawners) * ratio;
}

bool better(const Result &a, const Result &b, double conversion_ratio = 1.0);

class Ranking {
    std::map<int, std::vector<Result>> groups_;
    size_t capacity_;
    double conversion_ratio_;

  public:
    explicit Ranking(size_t capacity, double conversion_ratio = 1.0)
        : capacity_(capacity), conversion_ratio_(conversion_ratio) {}

    bool discard(int count, double upper) const;
    void accept(Result result);
    std::vector<Result> results() const;
    size_t size() const;
};

struct Progress {
    uint64_t completed{}, total{}, candidates{}, layouts{}, pruned{};
    uint64_t resumed{};
    // 已捕获到榜单的结果数（去重后），不等于候选或模拟数量。
    uint64_t found{};
    // 本次 bounds 实际覆盖的区块数；候选结构位置数另计在 candidates。
    uint64_t chunks{};
    // 搜索阶段实耗减暂停，含断点历史累计。
    double elapsed_ms{};
    double resumed_ms{};
};

struct Cancelled {};

class Control {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> paused_{};
    std::atomic<bool> cancelled_{};
    std::chrono::steady_clock::time_point paused_at_{};
    std::chrono::steady_clock::duration paused_time_{};

  public:
    void checkpoint();
    void pause();
    void resume();
    void cancel();

    bool running() const {
        return !paused_.load(std::memory_order_relaxed) && !cancelled_.load(std::memory_order_relaxed);
    }

    double elapsed_ms(std::chrono::steady_clock::time_point start);
    // 累计暂停毫秒，含当前尚未恢复的暂停段。
    double paused_ms();
};

struct SearchOutput {
    std::vector<Result> results;
    Progress stats;
    std::string checkpoint_path;
};

SearchOutput search(const Options &, Control &, const std::function<void(const Progress &)> &progress = {});
Result optimize_center(const Options &, const std::vector<Point> &, const std::vector<Vec3> &,
                       Control *control = nullptr);
}
