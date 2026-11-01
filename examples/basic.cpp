#include <minecraft_finders/trial.hpp>
#include <iostream>

int main() {
    trial::Options options;
    options.seed = 0;
    options.seed_text = "0";
    options.generation = trial::generation_for_version("26.3");
    options.radius = 1000;
    options.min_structures = 1;
    options.min_spawners = 0;
    options.threads = 1;
    trial::Control control;
    auto result = trial::search(options, control);
    std::cout << result.results.size() << '\n';
    return result.stats.completed == result.stats.total ? 0 : 1;
}
