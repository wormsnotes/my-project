#include "portfolio/portfolio.hpp"

#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/methods/montecarlo/multipathgenerator.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/processes/stochasticprocessarray.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>

namespace portfolio {
namespace {

constexpr double trading_days_per_year = 252.0;
constexpr std::size_t max_samples = 1000000;
using PathGenerator = QuantLib::MultiPathGenerator<QuantLib::PseudoRandom::rsg_type>;

void require_finite(double number, const char* name) {
    if (!std::isfinite(number)) {
        throw std::invalid_argument(std::string(name) + " must be finite");
    }
}

void validate_rates(const FxRates& rates) {
    require_finite(rates.rub_usd, "RUB price");
    require_finite(rates.eur_usd, "EUR price");
    if (rates.rub_usd <= 0.0 || rates.eur_usd <= 0.0) {
        throw std::invalid_argument("Currency prices must be positive");
    }
}

void validate_history(const History& history) {
    if (history.size() < 3) {
        throw std::invalid_argument("At least three price observations are required");
    }
    for (const auto& rates : history) {
        validate_rates(rates);
    }
}

void validate_sampling(std::size_t samples, std::uint32_t seed) {
    if (samples == 0 || samples > max_samples || seed == 0) {
        throw std::invalid_argument("Samples must be 1..1000000; seed must be nonzero");
    }
}

auto currency_process(double spot, double log_mean, double log_stddev) {
    using namespace QuantLib;
    // QuantLib принимает годовые параметры. Drift задаёт оценённое физическое
    // распределение, а не безрисковую ставку из задачи оценки опционов.
    const double annual_variance = log_stddev * log_stddev * trading_days_per_year;
    const double annual_drift = log_mean * trading_days_per_year + annual_variance / 2.0;
    const Date reference(1, January, 2025);
    const Actual365Fixed day_counter;
    const Handle<Quote> price(ext::make_shared<SimpleQuote>(spot));
    const Handle<YieldTermStructure> drift(
        ext::make_shared<FlatForward>(reference, annual_drift, day_counter));
    const Handle<BlackVolTermStructure> volatility(
        ext::make_shared<BlackConstantVol>(reference, NullCalendar(),
                                         std::sqrt(annual_variance), day_counter));
    return ext::make_shared<BlackScholesProcess>(price, drift, volatility);
}

PathGenerator path_generator(const FxModel& model, std::size_t days,
                             std::uint32_t seed) {
    validate_rates(model.spot);
    require_finite(model.rub_log_mean, "RUB mean");
    require_finite(model.eur_log_mean, "EUR mean");
    require_finite(model.rub_log_stddev, "RUB standard deviation");
    require_finite(model.eur_log_stddev, "EUR standard deviation");
    require_finite(model.correlation, "Correlation");
    if (model.rub_log_stddev < 0.0 || model.eur_log_stddev < 0.0 ||
        std::abs(model.correlation) > 1.0) {
        throw std::invalid_argument("Invalid volatility or correlation");
    }
    using namespace QuantLib;
    std::vector<ext::shared_ptr<StochasticProcess1D>> processes{
        currency_process(model.spot.rub_usd, model.rub_log_mean, model.rub_log_stddev),
        currency_process(model.spot.eur_usd, model.eur_log_mean, model.eur_log_stddev)};
    Matrix correlation(2, 2, model.correlation);
    correlation[0][0] = correlation[1][1] = 1.0;
    auto joint_process = ext::make_shared<StochasticProcessArray>(processes, correlation);
    return PathGenerator(joint_process,
                         TimeGrid(static_cast<double>(days) / trading_days_per_year, days),
                         PseudoRandom::make_sequence_generator(2 * days, seed));
}

double mean(const std::vector<double>& values) {
    return static_cast<double>(std::accumulate(values.begin(), values.end(), 0.0L) /
                               static_cast<long double>(values.size()));
}

double quantile(const std::vector<double>& sorted, double probability) {
    const double position = probability * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    return sorted[lower] + (sorted[upper] - sorted[lower]) *
                              (position - static_cast<double>(lower));
}

} // namespace

double value_usd(const Holdings& holdings, const FxRates& rates) {
    validate_rates(rates);
    for (double amount : {holdings.rub, holdings.usd, holdings.eur}) {
        require_finite(amount, "Currency amount");
        if (amount < 0.0) {
            throw std::invalid_argument("Currency amounts cannot be negative");
        }
    }
    const double result = holdings.rub * rates.rub_usd + holdings.usd +
                          holdings.eur * rates.eur_usd;
    if (!std::isfinite(result)) {
        throw std::overflow_error("Portfolio value is too large");
    }
    return result;
}

History generate_history(std::size_t days, std::uint32_t seed) {
    if (days < 2 || days > 10000 || seed == 0) {
        throw std::invalid_argument("History days must be 2..10000; seed must be nonzero");
    }
    FxModel synthetic;
    synthetic.rub_log_mean = -0.00010;
    synthetic.eur_log_mean = 0.00005;
    synthetic.rub_log_stddev = 0.20 / std::sqrt(trading_days_per_year);
    synthetic.eur_log_stddev = 0.10 / std::sqrt(trading_days_per_year);
    synthetic.correlation = 0.25;
    auto generator = path_generator(synthetic, days, seed);
    const auto& path = generator.next().value;
    History history;
    history.reserve(days + 1);
    for (std::size_t day = 0; day <= days; ++day) {
        history.push_back({path[0][day], path[1][day]});
    }
    return history;
}

FxModel fit_model(const History& history) {
    validate_history(history);
    std::vector<double> rub_returns;
    std::vector<double> eur_returns;
    for (std::size_t day = 1; day < history.size(); ++day) {
        // Разность логарифмов устойчива даже при очень разных масштабах цен.
        rub_returns.push_back(std::log(history[day].rub_usd) -
                              std::log(history[day - 1].rub_usd));
        eur_returns.push_back(std::log(history[day].eur_usd) -
                              std::log(history[day - 1].eur_usd));
    }
    FxModel model;
    model.spot = history.back();
    model.rub_log_mean = mean(rub_returns);
    model.eur_log_mean = mean(eur_returns);
    double rub_sum = 0.0;
    double eur_sum = 0.0;
    double covariance_sum = 0.0;
    for (std::size_t i = 0; i < rub_returns.size(); ++i) {
        const double rub_delta = rub_returns[i] - model.rub_log_mean;
        const double eur_delta = eur_returns[i] - model.eur_log_mean;
        rub_sum += rub_delta * rub_delta;
        eur_sum += eur_delta * eur_delta;
        covariance_sum += rub_delta * eur_delta;
    }
    const double denominator = static_cast<double>(rub_returns.size() - 1);
    model.rub_log_stddev = std::sqrt(rub_sum / denominator);
    model.eur_log_stddev = std::sqrt(eur_sum / denominator);
    if (rub_sum > 0.0 && eur_sum > 0.0) {
        model.correlation = std::clamp(covariance_sum / std::sqrt(rub_sum * eur_sum),
                                       -1.0, 1.0);
    }
    return model;
}

Forecast forecast(const Holdings& holdings, const FxModel& model,
                  std::size_t samples, std::uint32_t seed) {
    validate_sampling(samples, seed);
    Forecast result;
    result.current_usd = value_usd(holdings, model.spot);
    if (result.current_usd <= 0.0) {
        throw std::invalid_argument("Portfolio must have a positive value");
    }
    auto generator = path_generator(model, 1, seed);
    std::vector<double> values;
    values.reserve(samples);
    std::size_t losses = 0;
    const double rounding_tolerance = result.current_usd * 1e-12;
    for (std::size_t scenario = 0; scenario < samples; ++scenario) {
        const auto& path = generator.next().value;
        const double value = value_usd(holdings, {path[0][1], path[1][1]});
        values.push_back(value);
        losses += value < result.current_usd - rounding_tolerance;
    }
    result.mean_usd = mean(values);
    result.loss_probability = static_cast<double>(losses) / static_cast<double>(samples);
    std::sort(values.begin(), values.end());
    result.median_usd = quantile(values, 0.5);
    result.lower_usd = quantile(values, 0.025);
    result.upper_usd = quantile(values, 0.975);
    return result;
}

HypothesisTest test_decline(const Holdings& holdings, const History& history,
                           std::size_t samples, std::uint32_t seed, double alpha) {
    validate_sampling(samples, seed);
    validate_history(history);
    if (!std::isfinite(alpha) || alpha <= 0.0 || alpha >= 1.0) {
        throw std::invalid_argument("Alpha must be strictly between 0 and 1");
    }
    const double current = value_usd(holdings, history.back());
    if (current <= 0.0) {
        throw std::invalid_argument("Portfolio must have a positive value");
    }
    const double rub_weight = holdings.rub * history.back().rub_usd / current;
    const double eur_weight = holdings.eur * history.back().eur_usd / current;
    std::vector<double> returns;
    for (std::size_t day = 1; day < history.size(); ++day) {
        const double daily_return =
            rub_weight * (history[day].rub_usd / history[day - 1].rub_usd - 1.0) +
            eur_weight * (history[day].eur_usd / history[day - 1].eur_usd - 1.0);
        require_finite(daily_return, "Daily portfolio return");
        returns.push_back(daily_return);
    }
    HypothesisTest result;
    result.alpha = alpha;
    result.mean_daily_return = mean(returns);
    // H0 относится к среднему, а не к вероятности отдельного завтрашнего убытка.
    // Центрирование задаёт нулевое среднее в bootstrap-распределении при H0.
    for (auto& daily_return : returns) {
        daily_return -= result.mean_daily_return;
    }
    QuantLib::MersenneTwisterUniformRng random(seed);
    std::size_t lower_tail = 0;
    for (std::size_t trial = 0; trial < samples; ++trial) {
        long double sum = 0.0;
        for (std::size_t day = 0; day < returns.size(); ++day) {
            const auto index = static_cast<std::size_t>(
                random.next().value * static_cast<double>(returns.size()));
            sum += returns[index];
        }
        const double null_mean = static_cast<double>(sum / returns.size());
        lower_tail += null_mean <= result.mean_daily_return;
    }
    // Поправка +1 не позволяет объявить p-value равным нулю при конечном числе проб.
    result.p_value = static_cast<double>(lower_tail + 1) /
                     static_cast<double>(samples + 1);
    result.reject_null = result.p_value < alpha;
    return result;
}

void write_history_csv(const History& history, std::ostream& output) {
    validate_history(history);
    output << "day,RUB_USD,USD_USD,EUR_USD\n" << std::setprecision(17);
    for (std::size_t day = 0; day < history.size(); ++day) {
        output << day << ',' << history[day].rub_usd << ",1,"
               << history[day].eur_usd << '\n';
    }
    if (!output) {
        throw std::runtime_error("Could not write synthetic history");
    }
}

} // namespace portfolio
