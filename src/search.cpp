#include "search.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace trial {
using Clock = std::chrono::steady_clock;

void Control::checkpoint() {
    if (cancelled_.load(std::memory_order_relaxed))
        throw Cancelled{};
    if (!paused_.load(std::memory_order_relaxed))
        return;
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] {
        return !paused_ || cancelled_.load(std::memory_order_relaxed);
    });
    if (cancelled_.load(std::memory_order_relaxed))
        throw Cancelled{};
}

void Control::pause() {
    std::lock_guard lock(mutex_);
    if (!paused_) {
        paused_ = true;
        paused_at_ = Clock::now();
    }
}

void Control::resume() {
    std::lock_guard lock(mutex_);
    if (paused_) {
        paused_time_ += Clock::now() - paused_at_;
        paused_ = false;
    }
    changed_.notify_all();
}

void Control::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
    changed_.notify_all();
}

double Control::elapsed_ms(Clock::time_point start) {
    std::lock_guard lock(mutex_);
    return std::chrono::duration<double, std::milli>((paused_ ? paused_at_ : Clock::now()) - start - paused_time_)
        .count();
}

double Control::paused_ms() {
    std::lock_guard lock(mutex_);
    return std::chrono::duration<double, std::milli>(paused_time_ +
                                                     (paused_ ? Clock::now() - paused_at_ : Clock::duration{}))
        .count();
}

// 正式排名（契约 v4）：折算数量降序 → 距本次搜索中心水平距离平方升序 → X → Z。
// ratio=1.0 时折算数量等于总刷怪笼数，与旧 v3 排序完全一致。
bool better(const Result &a, const Result &b, double conversion_ratio) {
    const double ca = converted_spawners(a, conversion_ratio);
    const double cb = converted_spawners(b, conversion_ratio);
    if (ca != cb) return ca > cb;
    return std::tuple{a.distance_sq, a.x, a.z} < std::tuple{b.distance_sq, b.x, b.z};
}

static uint64_t key(int x, int z) {
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
}

// 统计窗口：圆、方、矩形三种。矩形按长边/短边各取一半作为闭区间半宽。
struct Window {
    int half_x, half_z;
    bool circle;
};

static Window cluster_window(const Options &o) {
    if (o.cluster_rect)
        return {std::max(0, o.cluster_long / 2), std::max(0, o.cluster_short / 2), false};
    return {o.cluster_radius, o.cluster_radius, !o.cluster_square};
}

// 严格模式把“结构起点”的窗口四边各缩进 strict_margin 格：clusters() 只用这个内窗口收集成员，
// 贴边的密室因此不进入该组（不模拟、不计结构数、不计笼子）；optimize_center 的笼子统计仍用完整窗口。
static Window structure_window(const Options &o) {
    auto window = cluster_window(o);
    if (!o.strict_mode)
        return window;
    window.half_x = std::max(0, window.half_x - o.strict_margin);
    window.half_z = std::max(0, window.half_z - o.strict_margin);
    return window;
}

// 某 |dz| 处的水平半宽：矩形恒为 half_x，圆形按 sqrt(half_x^2 - dz^2) 收缩。
static std::vector<int> horizontal_extent(int half_x, int half_z, bool circle) {
    std::vector<int> extent(size_t(half_z) + 1, half_x);
    if (!circle)
        return extent;
    for (int dz = 0; dz <= half_z; ++dz) {
        const int64_t value = int64_t(half_x) * half_x - int64_t(dz) * dz;
        int h = int(std::sqrt(double(value)));
        while (int64_t(h + 1) * (h + 1) <= value)
            ++h;
        while (int64_t(h) * h > value)
            --h;
        extent[dz] = h;
    }
    return extent;
}

static int cluster_max_half(const Options &o) {
    const auto window = cluster_window(o);
    return std::max({1, window.half_x, window.half_z});
}

struct IndexHash {
    size_t operator()(const std::vector<int> &values) const {
        size_t h = 0;
        for (int i : values)
            h ^= size_t(i) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class Grid {
    int size_;
    std::unordered_map<uint64_t, std::vector<int>> cells_;

  public:
    Grid(const std::vector<Point> &points, int size) : size_(size) {
        cells_.reserve(points.size());
        for (int i = 0; i < int(points.size()); ++i)
            cells_[key(floor_div(points[i].x, size_), floor_div(points[i].z, size_))].push_back(i);
    }

    template <class F> void nearby(int x, int z, F function) const {
        int cx = floor_div(x, size_), cz = floor_div(z, size_);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz) {
                auto it = cells_.find(key(cx + dx, cz + dz));
                if (it != cells_.end())
                    for (int i : it->second)
                        function(i);
            }
    }
};

struct Shard {
    int min_x, max_x, min_z, max_z;

    bool owns(Point p) const {
        return p.x >= min_x && p.x <= max_x && p.z >= min_z && p.z <= max_z;
    }
};

struct Bounds {
    int min_x, max_x, min_z, max_z;
};

// 搜索范围统一在这里求值：full_world 优先，其次精确四至，最后是旧的 center + radius。
static Bounds search_bounds(const Options &o) {
    constexpr int world_min = -30000000, world_max = 29999999;
    if (o.full_world)
        return {world_min, world_max, world_min, world_max};
    if (o.search_rect)
        return {std::clamp(o.min_x, world_min, world_max), std::clamp(o.max_x, world_min, world_max),
                std::clamp(o.min_z, world_min, world_max), std::clamp(o.max_z, world_min, world_max)};
    return {std::clamp(o.center_x - o.radius, world_min, world_max),
            std::clamp(o.center_x + o.radius, world_min, world_max),
            std::clamp(o.center_z - o.radius, world_min, world_max),
            std::clamp(o.center_z + o.radius, world_min, world_max)};
}

// 排名与分片调度使用的中心：精确四至取矩形中点，旧路径取 Options.center。
static int bounds_center_x(const Options &o, const Bounds &b) {
    return o.full_world ? o.center_x : o.search_rect ? int((int64_t(b.min_x) + b.max_x) / 2) : o.center_x;
}

static int bounds_center_z(const Options &o, const Bounds &b) {
    return o.full_world ? o.center_z : o.search_rect ? int((int64_t(b.min_z) + b.max_z) / 2) : o.center_z;
}

static std::vector<Shard> make_shards(const Options &o) {
    const auto bounds = search_bounds(o);
    int lx = bounds.min_x, hx = bounds.max_x, lz = bounds.min_z, hz = bounds.max_z;
    if (lx > hx || lz > hz)
        return {};
    const int center_x = bounds_center_x(o, bounds), center_z = bounds_center_z(o, bounds);
    double area = double(int64_t(hx) - lx + 1) * double(int64_t(hz) - lz + 1);
    // 增加独立任务以减少慢任务拖尾；原生实测中更大分片降低吞吐。
    int size = std::clamp(int(std::ceil(std::sqrt(area / (o.threads * 8.0)))), 16384, 262144);
    if (o.shard_size > 0)
        size = o.shard_size;
    std::vector<Shard> shards;
    for (int64_t x = lx; x <= hx; x += size)
        for (int64_t z = lz; z <= hz; z += size)
            shards.push_back(
                {int(x), int(std::min<int64_t>(hx, x + size - 1)), int(z), int(std::min<int64_t>(hz, z + size - 1))});
    std::sort(shards.begin(), shards.end(), [&](const Shard &a, const Shard &b) {
        auto distance = [&](const Shard &s) {
            int64_t x = std::clamp(center_x, s.min_x, s.max_x) - int64_t(center_x),
                    z = std::clamp(center_z, s.min_z, s.max_z) - int64_t(center_z);
            return x * x + z * z;
        };
        return std::tuple{distance(a), a.min_x, a.min_z} < std::tuple{distance(b), b.min_x, b.min_z};
    });
    return shards;
}

static std::vector<Point> enumerate(const Options &o, const Shard &s) {
    const auto bounds = search_bounds(o);
    int margin = cluster_max_half(o) * 2;
    int lx = std::max(-30000000, s.min_x - margin), hx = std::min(29999999, s.max_x + margin);
    int lz = std::max(-30000000, s.min_z - margin), hz = std::min(29999999, s.max_z + margin);
    std::vector<Point> points;
    for (int rx = floor_div(floor_div(lx, 16), 34) - 1; rx <= floor_div(floor_div(hx, 16), 34) + 1; ++rx)
        for (int rz = floor_div(floor_div(lz, 16), 34) - 1; rz <= floor_div(floor_div(hz, 16), 34) + 1; ++rz) {
            auto p = placement(o.seed, rx, rz);
            if (p.x < lx || p.x > hx || p.z < lz || p.z > hz)
                continue;
            if (!o.full_world) {
                if (o.search_rect) {
                    if (p.x < bounds.min_x || p.x > bounds.max_x || p.z < bounds.min_z || p.z > bounds.max_z)
                        continue;
                } else {
                    int64_t dx = int64_t(p.x) - o.center_x, dz = int64_t(p.z) - o.center_z;
                    if (o.search_square ? (std::abs(dx) > o.radius || std::abs(dz) > o.radius)
                                        : (dx * dx + dz * dz > int64_t(o.radius) * o.radius))
                        continue;
                }
            }
            points.push_back(p);
        }
    std::sort(points.begin(), points.end());
    return points;
}

static std::vector<std::vector<int>> clusters(const Options &o, const Shard &shard, const std::vector<Point> &points,
                                              Control &control) {
    // 严格模式下只有起点落在缩进窗口里的密室才算这一组的结构；贴边密室整间剔除，不参与结构数也不计笼子。
    // 笼子统计仍由 optimize_center 用完整窗口做，所以内窗口成员的笼子不会被边界裁掉。
    const auto window = structure_window(o);
    const int half_x = window.half_x, half_z = window.half_z;
    Grid grid(points, cluster_max_half(o) * 2);
    std::unordered_set<std::vector<int>, IndexHash> unique;
    std::vector<std::vector<int>> found;
    double radius2 = double(half_x) * half_x;
    auto accept = [&](double x, double z) {
        std::vector<int> members;
        members.reserve(4);
        grid.nearby(int(std::floor(x)), int(std::floor(z)), [&](int i) {
            double dx = points[i].x - x, dz = points[i].z - z;
            bool inside = window.circle ? (dx * dx + dz * dz <= radius2 + 1e-7)
                                        : (std::abs(dx) <= half_x && std::abs(dz) <= half_z);
            if (inside)
                members.push_back(i);
        });
        if (int(members.size()) < o.min_structures)
            return;
        std::sort(members.begin(), members.end());
        if (!shard.owns(points[members.front()]))
            return;
        if (window.circle) {
            bool legal = false;
            int fx = int(std::floor(x)), fz = int(std::floor(z));
            for (int ix = fx - 1; ix <= fx + 2 && !legal; ++ix)
                for (int iz = fz - 1; iz <= fz + 2 && !legal; ++iz) {
                    legal = true;
                    for (int i : members) {
                        int64_t dx = int64_t(ix) - points[i].x, dz = int64_t(iz) - points[i].z;
                        if (dx * dx + dz * dz > int64_t(half_x) * half_x) {
                            legal = false;
                            break;
                        }
                    }
                }
            if (!legal)
                return;
        }
        if (unique.insert(members).second)
            found.push_back(std::move(members));
    };
    if (window.circle) {
        for (int i = 0; i < int(points.size()); ++i) {
            if ((i & 1023) == 0)
                control.checkpoint();
            auto a = points[i];
            accept(a.x, a.z);
            grid.nearby(a.x, a.z, [&](int j) {
                if (j >= i)
                    return;
                auto b = points[j];
                double dx = b.x - double(a.x), dz = b.z - double(a.z), d2 = dx * dx + dz * dz;
                if (d2 == 0 || d2 > 4 * radius2 + 1e-7)
                    return;
                double distance = std::sqrt(d2), height = std::sqrt(std::max(0.0, radius2 - d2 / 4)),
                       mx = (a.x + double(b.x)) / 2, mz = (a.z + double(b.z)) / 2;
                accept(mx - dz / distance * height, mz + dx / distance * height);
                accept(mx + dz / distance * height, mz - dx / distance * height);
            });
        }
    } else {
        std::unordered_set<uint64_t> centers;
        for (int i = 0; i < int(points.size()); ++i) {
            if ((i & 1023) == 0)
                control.checkpoint();
            auto a = points[i];
            std::vector<int> xs, zs;
            int nearby_count = 0;
            grid.nearby(a.x, a.z, [&](int j) {
                auto p = points[j];
                if (std::abs(int64_t(p.x) - a.x) <= int64_t(half_x) * 2 &&
                    std::abs(int64_t(p.z) - a.z) <= int64_t(half_z) * 2) {
                    ++nearby_count;
                    xs.push_back(p.x - half_x);
                    xs.push_back(p.x + half_x);
                    zs.push_back(p.z - half_z);
                    zs.push_back(p.z + half_z);
                }
            });
            if (nearby_count < o.min_structures)
                continue;
            centers.insert(key(a.x, a.z));
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            std::sort(zs.begin(), zs.end());
            zs.erase(std::unique(zs.begin(), zs.end()), zs.end());
            for (int x : xs)
                for (int z : zs)
                    centers.insert(key(x, z));
        }
        for (auto c : centers)
            accept(int32_t(c >> 32), int32_t(c));
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a.size() != b.size() ? a.size() > b.size() : a < b;
    });
    return found;
}

Result optimize_center(const Options &o, const std::vector<Point> &structures, const std::vector<Vec3> &spawners,
                       Control *control) {
    const auto window = cluster_window(o);   // 完整统计窗口：只用于数笼子
    const auto fit = structure_window(o);    // 结构起点窗口：严格模式下比统计窗口各小 strict_margin
    const int half_x = window.half_x, half_z = window.half_z;
    const int fit_x = fit.half_x, fit_z = fit.half_z;
    int min_z = std::numeric_limits<int>::min(), max_z = std::numeric_limits<int>::max();
    for (auto p : structures) {
        min_z = std::max(min_z, p.z - fit_z);
        max_z = std::min(max_z, p.z + fit_z);
    }
    const auto fit_extent = horizontal_extent(fit_x, fit_z, fit.circle);
    const auto count_extent = horizontal_extent(half_x, half_z, window.circle);
    // 可行中心的 x 跨度不超过结构窗口宽度，差分数组按它开就够；笼子统计的半宽可以比它大，靠裁剪对齐。
    std::vector<int> difference(2 * fit_x + 3);
    Result best{0, 0, int(structures.size()), -1, structures};
    for (int z = min_z; z <= max_z; ++z) {
        if (control && ((z - min_z) & 15) == 0)
            control->checkpoint();
        int min_x = std::numeric_limits<int>::min(), max_x = std::numeric_limits<int>::max();
        for (auto p : structures) {
            int dz = std::abs(p.z - z);
            if (dz > fit_z) {
                min_x = 1;
                max_x = 0;
                break;
            }
            int h = fit_extent[dz];
            min_x = std::max(min_x, p.x - h);
            max_x = std::min(max_x, p.x + h);
        }
        if (min_x > max_x)
            continue;
        int width = max_x - min_x + 1;
        std::fill_n(difference.begin(), width + 1, 0);
        for (auto p : spawners) {
            int dz = std::abs(p.z - z);
            if (dz > half_z)
                continue;
            int h = count_extent[dz], from = std::max(min_x, p.x - h), to = std::min(max_x, p.x + h);
            if (from <= to) {
                ++difference[from - min_x];
                --difference[to - min_x + 1];
            }
        }
        int count = 0;
        for (int i = 0; i < width; ++i) {
            count += difference[i];
            int x = min_x + i;
            if (count > best.spawners || (count == best.spawners && std::pair{x, z} < std::pair{best.x, best.z})) {
                best.x = x;
                best.z = z;
                best.spawners = count;
            }
        }
    }
    return best; // spawners < 0 表示结构窗口内无可行中心（理论上严格模式下成员在候选中心已可行）
}

// 组容量沿用 ranking_capacity()：旧的固定 100 条上限会让「前 200」在同数量组内提前丢掉仍可能按距离胜出的候选。
bool Ranking::discard(int count, double upper) const {
    auto it = groups_.find(count);
    return it != groups_.end() && it->second.size() == capacity_ &&
           upper < converted_spawners(it->second.back(), conversion_ratio_);
}

void Ranking::accept(Result result) {
    auto &group = groups_[result.count];
    auto duplicate = std::find_if(group.begin(), group.end(), [&](const auto &x) {
        return x.structures == result.structures;
    });
    if (duplicate != group.end()) {
        if (!better(result, *duplicate, conversion_ratio_))
            return;
        group.erase(duplicate);
    } else if (group.size() == capacity_ && !better(result, group.back(), conversion_ratio_))
        return;
    auto at = std::lower_bound(group.begin(), group.end(), result,
                               [this](const Result &left, const Result &right) {
                                   return better(left, right, conversion_ratio_);
                               });
    group.insert(at, std::move(result));
    if (group.size() > capacity_)
        group.pop_back();
}

std::vector<Result> Ranking::results() const {
    std::vector<Result> out;
    for (auto &[n, g] : groups_)
        out.insert(out.end(), g.begin(), g.end());
    std::sort(out.begin(), out.end(), [this](const Result &a, const Result &b) {
        return better(a, b, conversion_ratio_);
    });
    return out;
}

size_t Ranking::size() const {
    size_t total = 0;
    for (auto &[n, g] : groups_)
        total += g.size();
    return total;
}

template <class T> static void write_value(std::ostream &stream, const T &value) {
    stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <class T> static T read_value(std::istream &stream) {
    T value{};
    stream.read(reinterpret_cast<char *>(&value), sizeof(value));
    return value;
}

class Checkpoint {
    std::filesystem::path path_;
    Clock::time_point saved_at_ = Clock::now();
    Control *control_ = nullptr;

  public:
    std::vector<uint64_t> completed;

    Checkpoint(const Options &o, size_t count, Control *control = nullptr)
        : control_(control), completed((count + 63) / 64) {
        if (o.checkpoint_directory.empty())
            return;
        uint64_t hash = 1469598103934665603ULL;
        // 分片划分由调度版本、线程数与范围决定；旧划分的完成位不能复用。
        // 排名规则版本与组容量也进指纹：排序或 Top K 容量变了，旧榜单不是新规则下的完整榜单。
        const uint64_t values[] = {5,
                                   o.seed,
                                   uint64_t(o.generation),
                                   uint64_t(o.center_x),
                                   uint64_t(o.center_z),
                                   uint64_t(o.radius),
                                   uint64_t(o.cluster_radius),
                                   uint64_t(o.shard_size),
                                   uint64_t(o.min_structures),
                                   uint64_t(o.min_spawners),
                                   uint64_t(o.threads),
                                   uint64_t(o.search_square),
                                   uint64_t(o.cluster_square),
                                   uint64_t(o.full_world),
                                   uint64_t(std::llround(o.conversion_ratio * 1e9)),
                                   uint64_t(ranking_rule_version),
                                   uint64_t(ranking_capacity(o.result_limit))};
        for (auto value : values)
            for (int i = 0; i < 8; ++i) {
                hash = (hash ^ (value & 255)) * 1099511628211ULL;
                value >>= 8;
            }
        // 精确四至/矩形统计窗口只在启用时追加盐值，保持旧 center+radius 与旧统计窗口的断点文件名不变。
        if (o.search_rect) {
            const uint64_t rect[] = {1, uint64_t(o.min_x), uint64_t(o.max_x), uint64_t(o.min_z), uint64_t(o.max_z)};
            for (auto value : rect)
                for (int i = 0; i < 8; ++i) {
                    hash = (hash ^ (value & 255)) * 1099511628211ULL;
                    value >>= 8;
                }
        }
        if (o.cluster_rect) {
            const uint64_t window[] = {2, uint64_t(o.cluster_long), uint64_t(o.cluster_short)};
            for (auto value : window)
                for (int i = 0; i < 8; ++i) {
                    hash = (hash ^ (value & 255)) * 1099511628211ULL;
                    value >>= 8;
                }
        }
        // 严格模式改变了可行中心，旧榜单不能复用；同样只在启用时加盐，普通模式的断点文件名保持不变。
        if (o.strict_mode) {
            const uint64_t mode[] = {3, uint64_t(o.strict_margin)};
            for (auto value : mode)
                for (int i = 0; i < 8; ++i) {
                    hash = (hash ^ (value & 255)) * 1099511628211ULL;
                    value >>= 8;
                }
        }
        // 联数上限会改变收录范围；只在启用时加盐，默认 0 的断点文件名保持不变。
        if (o.max_structures > 0) {
            const uint64_t mode[] = {4, uint64_t(o.max_structures)};
            for (auto value : mode)
                for (int i = 0; i < 8; ++i) {
                    hash = (hash ^ (value & 255)) * 1099511628211ULL;
                    value >>= 8;
                }
        }
        try {
            std::filesystem::create_directories(o.checkpoint_directory);
            path_ = std::filesystem::path(o.checkpoint_directory) /
                    ("native-v5-" + std::to_string(hash) + ".bin");
        } catch (const std::filesystem::filesystem_error &) {
            // 断点只是加速/续跑功能；目录不可写时退化为本次不写断点，不能因此让搜索失败。
            path_.clear();
            if (control_) control_->mark_checkpoint_failed();
        }
    }

    bool done(size_t index) const {
        return completed[index / 64] & (uint64_t(1) << (index % 64));
    }

    void mark(size_t index) {
        completed[index / 64] |= uint64_t(1) << (index % 64);
    }

    void load(Ranking &ranking, Progress &stats) {
        if (path_.empty())
            return;
        std::error_code existsError;
        if (!std::filesystem::exists(path_, existsError)) {
            if (existsError) path_.clear();
            return;
        }
        try {
            std::ifstream file(path_, std::ios::binary);
            if (read_value<uint32_t>(file) != 0x54534635 || read_value<uint64_t>(file) != completed.size())
                throw std::runtime_error("原生断点格式不匹配");
            std::vector<uint64_t> loadedCompleted(completed.size());
            file.read(reinterpret_cast<char *>(loadedCompleted.data()),
                      std::streamsize(loadedCompleted.size() * sizeof(uint64_t)));
            Progress loaded = stats;
            loaded.candidates = read_value<uint64_t>(file);
            loaded.layouts = read_value<uint64_t>(file);
            loaded.pruned = read_value<uint64_t>(file);
            loaded.elapsed_ms = read_value<double>(file);
            auto size = read_value<uint32_t>(file);
            if (size > 1000000)
                throw std::runtime_error("原生断点结果数异常");
            std::vector<Result> results;
            results.reserve(size);
            for (uint32_t i = 0; i < size; ++i) {
                Result r;
                r.x = read_value<int>(file);
                r.z = read_value<int>(file);
                r.count = read_value<int>(file);
                r.spawners = read_value<int>(file);
                r.fast_spawners = read_value<int>(file);
                r.slow_spawners = read_value<int>(file);
                r.distance_sq = read_value<int64_t>(file);
                if (r.count < 0 || r.count > 32)
                    throw std::runtime_error("原生断点结构数异常");
                for (int j = 0; j < r.count; ++j) {
                    Point p;
                    p.x = read_value<int>(file);
                    p.z = read_value<int>(file);
                    r.structures.push_back(p);
                }
                results.push_back(std::move(r));
            }
            if (!file)
                throw std::runtime_error("原生断点读取失败");
            completed = std::move(loadedCompleted);
            stats = loaded;
            for (auto word : completed)
                stats.completed += std::popcount(word);
            stats.resumed = stats.completed;
            for (auto &result : results)
                ranking.accept(std::move(result));
            stats.found = ranking.size();
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &) {
            // 旧版本或半截断点不能阻断本次搜索：删掉它并按全新任务继续。
            std::error_code removeError;
            std::filesystem::remove(path_, removeError);
            path_.clear();
            if (control_) control_->mark_checkpoint_failed();
        }
    }

    void save(const Ranking &ranking, const Progress &stats, bool force = false) {
        if (path_.empty() || (!force && Clock::now() - saved_at_ < std::chrono::seconds(1)))
            return;
        auto temporary = path_;
        temporary += ".tmp";
        try {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("原生断点写入失败");
            write_value(file, uint32_t(0x54534635));
            write_value(file, uint64_t(completed.size()));
            file.write(reinterpret_cast<const char *>(completed.data()),
                       std::streamsize(completed.size() * sizeof(uint64_t)));
            write_value(file, stats.candidates);
            write_value(file, stats.layouts);
            write_value(file, stats.pruned);
            write_value(file, stats.elapsed_ms);
            auto results = ranking.results();
            write_value(file, uint32_t(results.size()));
            for (auto &r : results) {
                write_value(file, r.x);
                write_value(file, r.z);
                write_value(file, r.count);
                write_value(file, r.spawners);
                write_value(file, r.fast_spawners);
                write_value(file, r.slow_spawners);
                write_value(file, r.distance_sq);
                for (auto p : r.structures) {
                    write_value(file, p.x);
                    write_value(file, p.z);
                }
            }
            file.flush();
            if (!file)
                throw std::runtime_error("原生断点写入失败");
            file.close();
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("原生断点替换失败");
#else
            std::filesystem::rename(temporary, path_);
#endif
            saved_at_ = Clock::now();
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &) {
            // 断点写入失败（目录只读、被杀软占用等）不应让整个搜索失败。
            std::error_code removeError;
            std::filesystem::remove(temporary, removeError);
            path_.clear();
            if (control_) control_->mark_checkpoint_failed();
        }
    }

    std::string path() const {
        return path_.string();
    }
};

// bounds 实际覆盖的区块数：区块矩形与范围相交即计入，与候选结构位置数区分开。
static int64_t floor_div64(int64_t value, int64_t denominator) {
    int64_t quotient = value / denominator;
    if ((value % denominator != 0) && ((value < 0) != (denominator < 0)))
        --quotient;
    return quotient;
}

static int64_t floor_sqrt64(int64_t value) {
    int64_t root = int64_t(std::sqrt(double(value)));
    while ((root + 1) * (root + 1) <= value)
        ++root;
    while (root * root > value)
        --root;
    return root;
}

static uint64_t covered_chunks(const Options &o) {
    const auto bounds = search_bounds(o);
    int lx = bounds.min_x, hx = bounds.max_x, lz = bounds.min_z, hz = bounds.max_z;
    if (lx > hx || lz > hz)
        return 0;
    int64_t x0 = floor_div64(lx, 16), x1 = floor_div64(hx, 16), z0 = floor_div64(lz, 16), z1 = floor_div64(hz, 16);
    // 矩形四至、方形搜索和 full_world 都按区块矩形计；只有旧的圆形路径要逐列取圆内跨度。
    if (o.full_world || o.search_rect || o.search_square)
        return uint64_t((x1 - x0 + 1) * (z1 - z0 + 1));
    // 圆形：逐区块列用列矩形到圆心的最近点求 z 跨度，不把外包方形的角算成覆盖。
    int64_t radius_sq = int64_t(o.radius) * o.radius, total = 0;
    for (int64_t chunk_x = x0; chunk_x <= x1; ++chunk_x) {
        int64_t left = chunk_x * 16, right = left + 15;
        int64_t dx = left > o.center_x ? left - o.center_x : (right < o.center_x ? o.center_x - right : 0);
        int64_t remainder = radius_sq - dx * dx;
        if (remainder < 0)
            continue;
        int64_t half = floor_sqrt64(remainder);
        total += floor_div64(int64_t(o.center_z) + half, 16) - floor_div64(int64_t(o.center_z) - half, 16) + 1;
    }
    return uint64_t(total);
}

SearchOutput search(const Options &o, Control &control, const std::function<void(const Progress &)> &progress) {
    auto start = Clock::now();
    auto shards = make_shards(o);
    const auto bounds = search_bounds(o);
    const int center_x = bounds_center_x(o, bounds), center_z = bounds_center_z(o, bounds);
    const auto window = cluster_window(o);
    const int half_x = window.half_x, half_z = window.half_z;
    std::atomic<size_t> next{};
    std::mutex merge;
    Ranking ranking(ranking_capacity(o.result_limit), o.conversion_ratio);
    Progress stats;
    stats.total = shards.size();
    stats.chunks = covered_chunks(o);
    std::exception_ptr failure;
    Checkpoint checkpoint(o, shards.size(), &control);
    checkpoint.load(ranking, stats);
    if (stats.resumed && progress)
        progress(stats);
    double restored_elapsed = stats.elapsed_ms;
    stats.resumed_ms = restored_elapsed;
    auto worker = [&] {
        try {
            LayoutEngine engine(o.generation, o.seed);
            for (;;) {
                control.checkpoint();
                size_t index = next.fetch_add(1);
                if (index >= shards.size())
                    break;
                {
                    std::lock_guard lock(merge);
                    if (checkpoint.done(index))
                        continue;
                }
                const auto &shard = shards[index];
                auto points = enumerate(o, shard);
                auto groups = clusters(o, shard, points, control);
                std::vector<uint8_t> needed(points.size());
                for (auto &group : groups)
                    for (int i : group)
                        needed[i] = 1;
                std::vector<Chamber> chambers(points.size());
                uint64_t layouts = 0, pruned = 0;
                for (int i = 0; i < int(points.size()); ++i)
                    if (needed[i]) {
                        control.checkpoint();
                        chambers[i] = engine.generate(floor_div(points[i].x, 16), floor_div(points[i].z, 16));
                        ++layouts;
                    }
                Ranking local(ranking_capacity(o.result_limit), o.conversion_ratio);
                for (auto &group : groups) {
                    control.checkpoint();
                    std::vector<Point> structures;
                    std::vector<Vec3> spawners;
                    std::vector<Vec3> slow_spawners;
                    for (int i : group)
                        if (!chambers[i].spawners.empty()) {
                            structures.push_back(points[i]);
                            auto &s = chambers[i].spawners;
                            spawners.insert(spawners.end(), s.begin(), s.end());
                            const auto &slow = chambers[i].slow_spawners;
                            slow_spawners.insert(slow_spawners.end(), slow.begin(), slow.end());
                        }
                    if (int(structures.size()) < o.min_structures)
                        continue;
                    // 联数上限：超过上限的簇整组剔除（不枚举子集），保证结果最多 max_structures 联。
                    if (o.max_structures > 0 && int(structures.size()) > o.max_structures)
                        continue;
                    // 这里只是用簇内去重前的总数做上界预筛：比率限制在 0–1，等效笼数不会超过总数，
                    // 所以“总数都不够”时必然不够。真正的阈值判断在 optimize_center 之后按等效笼数做。
                    if (int(spawners.size()) < o.min_spawners ||
                        local.discard(int(structures.size()), int(spawners.size()))) {
                        ++pruned;
                        continue;
                    }
                    {
                        std::lock_guard lock(merge);
                        if (ranking.discard(int(structures.size()), int(spawners.size()))) {
                            ++pruned;
                            continue;
                        }
                    }
                    std::sort(spawners.begin(), spawners.end());
                    spawners.erase(std::unique(spawners.begin(), spawners.end()), spawners.end());
                    auto result = optimize_center(o, structures, spawners, &control);
                    // 严格模式下这组密室起点无法全部离窗口边界 strict_margin 格时，没有可行中心。
                    if (result.spawners < 0)
                        continue;
                    // 正式排名用的距离参照本次任务实际 bounds.center；组内几何优化仍只看覆盖率。
                    result.distance_sq = squared_distance(result.x, result.z, center_x, center_z);
                    std::sort(slow_spawners.begin(), slow_spawners.end());
                    slow_spawners.erase(std::unique(slow_spawners.begin(), slow_spawners.end()),
                                        slow_spawners.end());
                    for (auto p : slow_spawners) {
                        int64_t dx = int64_t(p.x) - result.x;
                        int64_t dz = int64_t(p.z) - result.z;
                        bool inside = window.circle
                            ? (dx * dx + dz * dz <= int64_t(half_x) * half_x)
                            : (std::abs(dx) <= half_x && std::abs(dz) <= half_z);
                        if (inside)
                            ++result.slow_spawners;
                    }
                    result.fast_spawners = result.spawners - result.slow_spawners;
                    // 阈值口径与排名键一致：窗口内等效笼数（快 + 慢×比例），不再是快慢总数。
                    if (meets_min_spawners(result, o.min_spawners, o.conversion_ratio))
                        local.accept(std::move(result));
                }
                std::lock_guard lock(merge);
                for (auto &result : local.results())
                    ranking.accept(std::move(result));
                checkpoint.mark(index);
                ++stats.completed;
                stats.layouts += layouts;
                stats.pruned += pruned;
                for (auto p : points)
                    if (shard.owns(p))
                        ++stats.candidates;
                stats.found = ranking.size();
                stats.elapsed_ms = restored_elapsed + control.elapsed_ms(start);
                checkpoint.save(ranking, stats);
                if (progress)
                    progress(stats);
            }
        } catch (...) {
            std::lock_guard lock(merge);
            if (!failure)
                failure = std::current_exception();
            control.cancel();
        }
    };
    int count = std::min<size_t>(o.threads, std::max<size_t>(1, shards.size()));
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i)
        threads.emplace_back(worker);
    for (auto &thread : threads)
        thread.join();
    if (failure) {
        checkpoint.save(ranking, stats, true);
        std::rethrow_exception(failure);
    }
    stats.found = ranking.size();
    stats.elapsed_ms = restored_elapsed + control.elapsed_ms(start);
    checkpoint.save(ranking, stats, true);
    auto results = ranking.results();
    if (o.result_limit > 0 && results.size() > size_t(o.result_limit))
        results.resize(o.result_limit);
    // 终态进度里的 foundCandidates 与返回条数一致；搜索过程中的帧仍是榜单已捕获数。
    stats.found = results.size();
    return {std::move(results), stats, checkpoint.path()};
}
}
