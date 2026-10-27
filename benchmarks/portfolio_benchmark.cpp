#include "portfolio/portfolio.hpp"

#include <benchmark/benchmark.h>

// Генерация истории и оценка параметров находятся за пределами измеряемого цикла.
static void forecast_next_day(benchmark::State& state) {
    const auto model = portfolio::fit_model(portfolio::generate_history());
    const portfolio::Holdings holdings{100000.0, 1000.0, 500.0};
    const auto samples = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        auto result = portfolio::forecast(holdings, model, samples, 42);
        benchmark::DoNotOptimize(result.mean_usd);
        benchmark::DoNotOptimize(result.loss_probability);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(samples));
}

BENCHMARK(forecast_next_day)->Arg(1000)->Arg(10000)->Arg(100000)
                         ->MinTime(0.1)->Unit(benchmark::kMillisecond);
