cmake_minimum_required(VERSION 3.15)
project(nanobucket VERSION 0.1.0 LANGUAGES CXX)

# ── Language standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json for clangd / IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies (resolved by Conan CMakeDeps) ───────────────────────────────
find_package(Drogon  REQUIRED)
find_package(spdlog  REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(pugixml REQUIRED)
find_package(SQLite3 REQUIRED)   # target: SQLite::SQLite3
find_package(GTest   REQUIRED)   # targets: GTest::gtest, GTest::gtest_main

# ── Sources ──────────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(test)
