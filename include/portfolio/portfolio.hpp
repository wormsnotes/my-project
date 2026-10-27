#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

/** @file portfolio.hpp
 *  @brief Денежный портфель, синтетическая история и однодневная оценка риска.
 */

/// Расчёты денежного портфеля в долларах США.
namespace portfolio {

/// Количество денежных единиц каждой валюты; заёмные позиции не поддерживаются.
struct Holdings {
    double rub = 0.0; ///< Количество рублей.
    double usd = 0.0; ///< Количество долларов.
    double eur = 0.0; ///< Количество евро.
};

/// Цена одной единицы валюты в долларах; цена USD всегда равна 1.
struct FxRates {
    double rub_usd = 0.01; ///< USD за 1 RUB.
    double eur_usd = 1.10; ///< USD за 1 EUR.
};

/// Последовательные наблюдения: начальная цена и цены в конце торговых дней.
using History = std::vector<FxRates>;

/// Параметры совместного нормального распределения дневных логарифмов доходности.
struct FxModel {
    FxRates spot; ///< Последнее наблюдение истории.
    double rub_log_mean = 0.0; ///< Средний log(RUB_USD[t] / RUB_USD[t-1]).
    double eur_log_mean = 0.0; ///< Средний log(EUR_USD[t] / EUR_USD[t-1]).
    double rub_log_stddev = 0.0; ///< Стандартное отклонение дневной доходности RUB.
    double eur_log_stddev = 0.0; ///< Стандартное отклонение дневной доходности EUR.
    double correlation = 0.0; ///< Корреляция дневных логарифмов доходности.
};

/// Оценки по независимым сценариям следующего торгового дня.
struct Forecast {
    double current_usd = 0.0; ///< Стоимость портфеля сейчас.
    double mean_usd = 0.0; ///< Средняя стоимость в сценариях.
    double median_usd = 0.0; ///< Медиана стоимости.
    double lower_usd = 0.0; ///< Квантиль 2.5%: нижняя граница модельного интервала.
    double upper_usd = 0.0; ///< Квантиль 97.5%: верхняя граница модельного интервала.
    double loss_probability = 0.0; ///< Доля сценариев со снижением стоимости.
};

/// Результат односторонней bootstrap-проверки среднего дневного изменения.
struct HypothesisTest {
    double mean_daily_return = 0.0; ///< Средняя историческая доходность при текущих весах.
    double p_value = 1.0; ///< Оценка вероятности статистики не выше наблюдаемой при H0.
    double alpha = 0.05; ///< Уровень значимости.
    bool reject_null = false; ///< Есть ли основания отвергнуть H0 в пользу снижения.
};

/** Стоимость RUB + USD + EUR в долларах.
 *  @throws std::invalid_argument При отрицательных суммах или некорректных ценах.
 *  @throws std::overflow_error Если результат не помещается в double.
 */
double value_usd(const Holdings& holdings, const FxRates& rates);

/** Создаёт правдоподобные учебные цены: 252 торговых дня и начальное наблюдение.
 *  Используется готовый генератор коррелированных траекторий QuantLib.
 *  @param days Число торговых дней, не менее двух.
 *  @param seed Фиксированное зерно генератора; нулевое зерно запрещено.
 */
History generate_history(std::size_t days = 252, std::uint32_t seed = 2026);

/** Оценивает средние, стандартные отклонения и корреляцию по истории.
 *  @throws std::invalid_argument Если наблюдений меньше трёх или цены некорректны.
 */
FxModel fit_model(const History& history);

/** Моделирует стоимость на следующий торговый день, не меняя количества валют.
 *  @param holdings Количество валют; общая стоимость должна быть положительной.
 *  @param model Оценённая модель дневных логарифмов доходности.
 *  @param samples Количество сценариев от 1 до 1000000.
 *  @param seed Ненулевое зерно генератора QuantLib.
 */
Forecast forecast(const Holdings& holdings, const FxModel& model,
                  std::size_t samples = 10000, std::uint32_t seed = 42);

/** Проверяет H0: средняя дневная доходность равна 0 против H1: она меньше 0.
 *  Выборка — исторические дневные доходности с сегодняшними весами валют.
 *  Bootstrap-выборки строятся из центрированной истории, то есть под H0.
 *  Предполагается независимость и одинаковое распределение дневных доходностей.
 *  @param holdings Количество валют в оцениваемом портфеле.
 *  @param history Исторические цены, минимум три наблюдения.
 *  @param samples Количество повторных выборок от 1 до 1000000.
 *  @param seed Ненулевое зерно отдельного генератора.
 *  @param alpha Уровень значимости строго между 0 и 1.
 */
HypothesisTest test_decline(const Holdings& holdings, const History& history,
                           std::size_t samples = 10000, std::uint32_t seed = 43,
                           double alpha = 0.05);

/// Записывает историю в CSV: day,RUB_USD,USD_USD,EUR_USD.
void write_history_csv(const History& history, std::ostream& output);

} // namespace portfolio
