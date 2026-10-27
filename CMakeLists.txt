cmake_minimum_required(VERSION 3.20)
project(PortfolioRisk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(QUANTLIB REQUIRED IMPORTED_TARGET quantlib)

add_library(portfolio_core src/portfolio.cpp)
target_include_directories(portfolio_core PUBLIC include)
target_link_libraries(portfolio_core PRIVATE PkgConfig::QUANTLIB)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(portfolio_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(portfolio-risk src/main.cpp)
target_link_libraries(portfolio-risk PRIVATE portfolio_core)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(portfolio_tests tests/portfolio_test.cpp)
    target_link_libraries(portfolio_tests PRIVATE portfolio_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(portfolio_tests)
    add_test(NAME cli_usd_only COMMAND portfolio-risk --usd 1000 --samples 100)
    set_tests_properties(cli_usd_only PROPERTIES
        PASS_REGULAR_EXPRESSION "Вероятность убытка по модели: 0[.]00%")
    add_test(NAME cli_rejects_negative COMMAND portfolio-risk --rub -1)
    add_test(NAME cli_rejects_nan COMMAND portfolio-risk --usd nan)
    add_test(NAME cli_rejects_bad_samples COMMAND portfolio-risk --usd 1 --samples -1)
    set_tests_properties(cli_rejects_negative cli_rejects_nan cli_rejects_bad_samples
                         PROPERTIES WILL_FAIL TRUE)
endif()

option(BUILD_BENCHMARKS "Build Google Benchmark performance measurements" ON)
if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)
    add_executable(portfolio_benchmark benchmarks/portfolio_benchmark.cpp)
    target_link_libraries(portfolio_benchmark PRIVATE portfolio_core benchmark::benchmark_main)
endif()

option(BUILD_DOCUMENTATION "Enable Doxygen documentation target" ON)
if(BUILD_DOCUMENTATION)
    find_package(Doxygen REQUIRED)
    configure_file(docs/Doxyfile.in Doxyfile @ONLY)
    add_custom_target(docs
        COMMAND Doxygen::doxygen "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generate Doxygen HTML documentation"
        VERBATIM)
endif()
