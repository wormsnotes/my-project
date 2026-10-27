#include "portfolio/portfolio.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    portfolio::Holdings holdings;
    std::size_t samples = 10000;
    std::uint32_t seed = 42;
    double alpha = 0.05;
    std::string history_output;
};

double number(const std::string& text) {
    std::size_t consumed = 0;
    const double result = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(result)) {
        throw std::invalid_argument("Expected a finite number: " + text);
    }
    return result;
}

std::uint64_t integer(const std::string& text) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        throw std::invalid_argument("Expected an unsigned integer: " + text);
    }
    return std::stoull(text);
}

void help() {
    std::cout << "Использование:\n"
              << "  ./build/portfolio-risk --rub 100000 --usd 1000 --eur 500\n"
              << "  ./build/portfolio-risk  (ввод сумм с клавиатуры)\n\n"
              << "Параметры:\n"
              << "  --rub, --usd, --eur  Количество денег; пропущенные валюты равны 0\n"
              << "  --samples N         Число сценариев и bootstrap-повторов (1..1000000)\n"
              << "  --seed N            Seed прогноза (1..4294967294), по умолчанию 42\n"
              << "  --alpha X           Уровень значимости, по умолчанию 0.05\n"
              << "  --history-out FILE  Сохранить сгенерированную историю в CSV\n"
              << "  --help              Показать справку\n\n"
              << "Дробные числа вводятся с точкой. История синтетическая, её seed = 2026.\n";
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    if (argc == 1) {
        std::cout << "Введите количество рублей, долларов и евро через пробел: "
                  << std::flush;
        std::string rub;
        std::string usd;
        std::string eur;
        if (!(std::cin >> rub >> usd >> eur)) {
            throw std::invalid_argument("Three currency amounts are required");
        }
        options.holdings = {number(rub), number(usd), number(eur)};
        return options;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 == argc) {
            throw std::invalid_argument("Missing value for " + flag);
        }
        const std::string value = argv[++i];
        if (flag == "--rub") {
            options.holdings.rub = number(value);
        } else if (flag == "--usd") {
            options.holdings.usd = number(value);
        } else if (flag == "--eur") {
            options.holdings.eur = number(value);
        } else if (flag == "--samples") {
            const auto samples = integer(value);
            if (samples == 0 || samples > 1000000) {
                throw std::invalid_argument("Samples must be between 1 and 1000000");
            }
            options.samples = static_cast<std::size_t>(samples);
        } else if (flag == "--seed") {
            const auto seed = integer(value);
            if (seed == 0 || seed >= std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("Seed must be between 1 and 4294967294");
            }
            options.seed = static_cast<std::uint32_t>(seed);
        } else if (flag == "--alpha") {
            options.alpha = number(value);
        } else if (flag == "--history-out") {
            options.history_output = value;
        } else {
            throw std::invalid_argument("Unknown option: " + flag);
        }
    }
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--help") {
                help();
                return 0;
            }
        }
        const Options options = parse_options(argc, argv);
        // Проверяем суммы до запуска моделирования.
        if (portfolio::value_usd(options.holdings, {}) <= 0.0) {
            throw std::invalid_argument("At least one amount must be positive");
        }
        const auto history = portfolio::generate_history();
        const auto model = portfolio::fit_model(history);
        const auto result = portfolio::forecast(options.holdings, model,
                                                 options.samples, options.seed);
        const auto test = portfolio::test_decline(options.holdings, history,
                                                  options.samples, options.seed + 1,
                                                  options.alpha);
        if (!options.history_output.empty()) {
            std::ofstream output(options.history_output);
            if (!output) {
                throw std::runtime_error("Cannot open history output: " + options.history_output);
            }
            portfolio::write_history_csv(history, output);
            output.close();
            if (!output) {
                throw std::runtime_error("Cannot save history output: " + options.history_output);
            }
        }

        std::cout << "СИНТЕТИЧЕСКИЕ данные: 252 торговых дня, seed истории = 2026.\n"
                  << "Сценариев: " << options.samples << "; seed прогноза = "
                  << options.seed << ".\n"
                  << std::fixed << std::setprecision(6)
                  << "Сегодня 1 RUB = " << model.spot.rub_usd
                  << " USD; 1 EUR = " << model.spot.eur_usd << " USD; 1 USD = 1 USD.\n\n"
                  << std::setprecision(2)
                  << "Стоимость сейчас: " << result.current_usd << " USD\n"
                  << "Средняя стоимость завтра по модели: " << result.mean_usd << " USD\n"
                  << "Среднее изменение: " << result.mean_usd - result.current_usd << " USD\n"
                  << "Медиана стоимости завтра: " << result.median_usd << " USD\n"
                  << "95% модельный интервал стоимости завтра: [" << result.lower_usd
                  << "; " << result.upper_usd << "] USD\n"
                  << "Вероятность убытка по модели: " << result.loss_probability * 100.0
                  << "%\n\n"
                  << "H0: ожидаемая стоимость за день не меняется; H1: снижается.\n"
                  << std::setprecision(6)
                  << "p-value = " << test.p_value << "; alpha = " << test.alpha << '\n';
        if (test.reject_null) {
            std::cout << "H0 отвергается: история даёт основания для вывода о снижении среднего.\n";
        } else {
            std::cout << "H0 не отвергается: оснований для вывода о снижении среднего недостаточно.\n";
        }
        std::cout << "Результат относится к учебной модели и не определяет фактический завтрашний курс.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка: " << error.what() << "\nДля справки: --help\n";
        return 1;
    }
}
