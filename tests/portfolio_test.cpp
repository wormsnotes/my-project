#include "portfolio/portfolio.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

using namespace portfolio;

History trend_history(double daily_change) {
    History history{{0.01, 1.10}};
    for (int day = 0; day < 40; ++day) {
        const double noise = day % 2 == 0 ? -0.001 : 0.001;
        history.push_back({history.back().rub_usd * (1.0 + daily_change + noise), 1.10});
    }
    return history;
}

TEST(Valuation, ConvertsUnitsToDollars) {
    EXPECT_DOUBLE_EQ(value_usd({10000.0, 100.0, 200.0}, {0.01, 1.10}), 420.0);
    EXPECT_DOUBLE_EQ(value_usd({}, {}), 0.0);
}

TEST(Valuation, RejectsInvalidAmountsAndPrices) {
    EXPECT_THROW(value_usd({-1.0, 0.0, 0.0}, {}), std::invalid_argument);
    EXPECT_THROW(value_usd({0.0, std::numeric_limits<double>::infinity(), 0.0}, {}),
                 std::invalid_argument);
    EXPECT_THROW(value_usd({}, {0.0, 1.10}), std::invalid_argument);
    EXPECT_THROW(value_usd({}, {0.01, std::numeric_limits<double>::quiet_NaN()}),
                 std::invalid_argument);
    EXPECT_THROW(value_usd({0.0, 0.0, std::numeric_limits<double>::max()}, {0.01, 2.0}),
                 std::overflow_error);
}

TEST(HistoryGeneration, ProducesOneYearOfPositiveReproduciblePrices) {
    const auto first = generate_history();
    const auto second = generate_history();
    ASSERT_EQ(first.size(), 253U);
    EXPECT_DOUBLE_EQ(first.front().rub_usd, 0.01);
    EXPECT_DOUBLE_EQ(first.front().eur_usd, 1.10);
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_GT(first[i].rub_usd, 0.0);
        EXPECT_GT(first[i].eur_usd, 0.0);
        EXPECT_DOUBLE_EQ(first[i].rub_usd, second[i].rub_usd);
        EXPECT_DOUBLE_EQ(first[i].eur_usd, second[i].eur_usd);
    }
    EXPECT_NE(first.back().rub_usd, generate_history(252, 2027).back().rub_usd);
}

TEST(HistoryGeneration, RejectsInvalidDayCountAndZeroSeed) {
    EXPECT_THROW(generate_history(1), std::invalid_argument);
    EXPECT_THROW(generate_history(252, 0), std::invalid_argument);
}

TEST(Calibration, RecoversKnownLogReturnsAndTheirCorrelation) {
    const History history{{1.0, 1.0}, {std::exp(0.01), std::exp(-0.02)},
                          {std::exp(0.04), 1.0}};
    const auto model = fit_model(history);
    EXPECT_NEAR(model.rub_log_mean, 0.02, 1e-12);
    EXPECT_NEAR(model.eur_log_mean, 0.0, 1e-12);
    EXPECT_NEAR(model.rub_log_stddev, std::sqrt(0.0002), 1e-12);
    EXPECT_NEAR(model.eur_log_stddev, std::sqrt(0.0008), 1e-12);
    EXPECT_NEAR(model.correlation, 1.0, 1e-12);
    EXPECT_DOUBLE_EQ(model.spot.rub_usd, history.back().rub_usd);
}

TEST(Calibration, HandlesConstantSeriesAndRejectsBadHistory) {
    const auto model = fit_model(History(5, {0.01, 1.10}));
    EXPECT_DOUBLE_EQ(model.rub_log_stddev, 0.0);
    EXPECT_DOUBLE_EQ(model.correlation, 0.0);
    EXPECT_THROW(fit_model(History(2)), std::invalid_argument);
    EXPECT_THROW(fit_model({{0.01, 1.1}, {-0.01, 1.1}, {0.01, 1.1}}),
                 std::invalid_argument);
}

TEST(Forecast, DollarOnlyPortfolioCannotHaveCurrencyLoss) {
    const auto model = fit_model(generate_history());
    const auto result = forecast({0.0, 1000.0, 0.0}, model, 1000, 42);
    EXPECT_DOUBLE_EQ(result.current_usd, 1000.0);
    EXPECT_DOUBLE_EQ(result.mean_usd, 1000.0);
    EXPECT_DOUBLE_EQ(result.lower_usd, 1000.0);
    EXPECT_DOUBLE_EQ(result.upper_usd, 1000.0);
    EXPECT_DOUBLE_EQ(result.loss_probability, 0.0);
}

TEST(Forecast, ConstantRatesPreserveAMixedPortfolio) {
    const auto model = fit_model(History(5, {0.01, 1.10}));
    const auto result = forecast({10000.0, 100.0, 200.0}, model, 1000, 7);
    EXPECT_NEAR(result.mean_usd, 420.0, 1e-10);
    EXPECT_NEAR(result.lower_usd, 420.0, 1e-10);
    EXPECT_DOUBLE_EQ(result.loss_probability, 0.0);
}

TEST(Forecast, MatchesAnalyticalLognormalMeanAndLossProbability) {
    FxModel model;
    model.spot.rub_usd = 100.0;
    model.rub_log_mean = 0.001;
    model.rub_log_stddev = 0.02;
    const auto result = forecast({1.0, 0.0, 0.0}, model, 50000, 123);
    const double expected = 100.0 * std::exp(0.001 + 0.02 * 0.02 / 2.0);
    // Допуски учитывают ошибку Монте-Карло, а не округление результата.
    EXPECT_NEAR(result.mean_usd, expected, 0.06);
    const double loss_probability = 0.5 * std::erfc(0.001 / (0.02 * std::sqrt(2.0)));
    EXPECT_NEAR(result.loss_probability, loss_probability, 0.015);
    EXPECT_NEAR(result.lower_usd, 100.0 * std::exp(0.001 - 1.959964 * 0.02), 0.15);
    EXPECT_NEAR(result.upper_usd, 100.0 * std::exp(0.001 + 1.959964 * 0.02), 0.15);
    EXPECT_LT(result.lower_usd, result.median_usd);
    EXPECT_LT(result.median_usd, result.upper_usd);
}

TEST(Forecast, CorrelationChangesPortfolioDispersion) {
    FxModel model;
    model.spot = {1.0, 1.0};
    model.rub_log_stddev = model.eur_log_stddev = 0.02;
    model.correlation = 0.9;
    const auto positive = forecast({1.0, 0.0, 1.0}, model, 10000, 77);
    model.correlation = -0.9;
    const auto negative = forecast({1.0, 0.0, 1.0}, model, 10000, 77);
    EXPECT_GT(positive.upper_usd - positive.lower_usd,
              2.0 * (negative.upper_usd - negative.lower_usd));
}

TEST(Forecast, FixedSeedIsReproducible) {
    const auto model = fit_model(generate_history());
    const auto first = forecast({10000.0, 100.0, 200.0}, model, 1000, 9);
    const auto second = forecast({10000.0, 100.0, 200.0}, model, 1000, 9);
    EXPECT_DOUBLE_EQ(first.mean_usd, second.mean_usd);
    EXPECT_DOUBLE_EQ(first.loss_probability, second.loss_probability);
    EXPECT_DOUBLE_EQ(first.lower_usd, second.lower_usd);
}

TEST(Forecast, RejectsEmptyPortfolioAndInvalidSimulationParameters) {
    FxModel model;
    EXPECT_THROW(forecast({}, model), std::invalid_argument);
    EXPECT_THROW(forecast({0, 1, 0}, model, 0), std::invalid_argument);
    EXPECT_THROW(forecast({0, 1, 0}, model, 100, 0), std::invalid_argument);
    model.correlation = 1.1;
    EXPECT_THROW(forecast({0, 1, 0}, model), std::invalid_argument);
    model.correlation = 0.0;
    model.rub_log_stddev = -0.1;
    EXPECT_THROW(forecast({0, 1, 0}, model), std::invalid_argument);
}

TEST(Hypothesis, DecliningHistoryRejectsTheZeroMeanNull) {
    const auto test = test_decline({10000, 0, 0}, trend_history(-0.005), 2000, 17);
    EXPECT_LT(test.mean_daily_return, 0.0);
    EXPECT_LT(test.p_value, 0.05);
    EXPECT_GT(test.p_value, 0.0);
    EXPECT_TRUE(test.reject_null);
}

TEST(Hypothesis, GrowingHistoryDoesNotSupportTheDecliningAlternative) {
    const auto test = test_decline({10000, 0, 0}, trend_history(0.005), 2000, 17);
    EXPECT_GT(test.mean_daily_return, 0.0);
    EXPECT_GT(test.p_value, 0.95);
    EXPECT_FALSE(test.reject_null);
}

TEST(Hypothesis, SymmetricHistoryIsNotEvidenceOfNegativeMean) {
    const auto history = trend_history(0.0);
    const auto test = test_decline({10000, 0, 0}, history, 4000, 17);
    EXPECT_NEAR(test.mean_daily_return, 0.0, 1e-14);
    EXPECT_GT(test.p_value, 0.4);
    EXPECT_FALSE(test.reject_null);
    // Больше искусственных сценариев не превращают ту же историю в новые данные.
    EXPECT_FALSE(test_decline({10000, 0, 0}, history, 8000, 17).reject_null);
}

TEST(Hypothesis, DollarOnlyAndUnchangingPricesHaveNoEvidenceOfDecline) {
    const auto dollars = test_decline({0, 1000, 0}, generate_history(), 1000, 17);
    EXPECT_DOUBLE_EQ(dollars.p_value, 1.0);
    EXPECT_FALSE(dollars.reject_null);
    const auto flat = test_decline({10000, 100, 200}, History(5), 1000, 17);
    EXPECT_DOUBLE_EQ(flat.p_value, 1.0);
    EXPECT_FALSE(flat.reject_null);
}

TEST(Hypothesis, RejectsInvalidAlphaAndPortfolio) {
    const auto history = trend_history(0.0);
    EXPECT_THROW(test_decline({0, 1, 0}, history, 100, 17, 0.0), std::invalid_argument);
    EXPECT_THROW(test_decline({0, 1, 0}, history, 100, 17, 1.0), std::invalid_argument);
    EXPECT_THROW(test_decline({}, history, 100, 17), std::invalid_argument);
}

TEST(Csv, WritesDollarDenominatedPricesAndExplicitUnitDollarPrice) {
    std::ostringstream output;
    write_history_csv(History(3, {0.01, 1.10}), output);
    EXPECT_EQ(output.str().find("day,RUB_USD,USD_USD,EUR_USD\n"), 0U);
    EXPECT_NE(output.str().find(",1,"), std::string::npos);
    std::istringstream lines(output.str());
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        ++count;
    }
    EXPECT_EQ(count, 4);
}

} // namespace
