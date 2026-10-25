cmake_minimum_required(VERSION 3.20)
project(trial_spawner_native LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CUBIOMES third_party/cubiomes)
add_library(biomes STATIC src/biome_bridge.c ${CUBIOMES}/generator.c ${CUBIOMES}/biomenoise.c ${CUBIOMES}/biomes.c
                          ${CUBIOMES}/layers.c ${CUBIOMES}/noise.c)
target_include_directories(biomes PUBLIC ${CUBIOMES})
add_library(trial_layout STATIC src/layout.cpp src/search.cpp src/climate263.cpp)
target_include_directories(trial_layout PUBLIC src data)
# 桌面契约 SearchResult.engineVersion；协议/排名规则或二进制行为变化时同步。
target_compile_definitions(trial_layout PUBLIC TRIAL_ENGINE_VERSION="3.0.0")
target_link_libraries(trial_layout PUBLIC biomes)
add_executable(trial-spawner src/main.cpp)
target_include_directories(trial-spawner PRIVATE third_party)
target_link_libraries(trial-spawner PRIVATE trial_layout)
if(MSVC)
    set_property(TARGET biomes trial_layout trial-spawner PROPERTY MSVC_RUNTIME_LIBRARY
                                                                   "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(trial-spawner PRIVATE /utf-8)
    target_compile_options(trial_layout PRIVATE /O2 /fp:strict /utf-8)
else()
    target_compile_options(biomes PRIVATE -O3 -fwrapv -ffp-contract=off -ffunction-sections -fdata-sections)
    target_compile_options(trial_layout PRIVATE -O3 -Wall -Wextra -ffp-contract=off -ffunction-sections -fdata-sections)
    target_link_options(
        trial-spawner
        PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -Wl,--gc-sections
        -s)
    if(WIN32)
        target_link_options(trial-spawner PRIVATE -municode)
    endif()
endif()
option(BUILD_TESTING "Build local regression tests" OFF)
include(CTest)
if(BUILD_TESTING)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit.cpp")
        message(FATAL_ERROR "Local tests/ is not included in this repository. Use -DBUILD_TESTING=OFF to build the engine.")
    endif()
    add_executable(trial-climate-check tests/climate_probe.cpp)
    target_link_libraries(trial-climate-check PRIVATE trial_layout)
    if(MSVC)
        set_property(TARGET trial-climate-check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(trial-climate-check PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_executable(trial-search-bench tests/search_bench.cpp)
    target_compile_definitions(trial-search-bench PRIVATE TRIAL_TUNE_SHARDS)
    target_link_libraries(trial-search-bench PRIVATE trial_layout)
    add_executable(trial-layout-corpus tests/layout_corpus.cpp)
    target_link_libraries(trial-layout-corpus PRIVATE trial_layout)
    add_executable(trial-unit tests/unit.cpp tests/biome_reference.c)
    target_link_libraries(trial-unit PRIVATE trial_layout)
    if(MSVC)
        set_property(TARGET trial-unit trial-search-bench trial-layout-corpus
                     PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(trial-unit PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(trial-search-bench PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(trial-layout-corpus PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME unit COMMAND trial-unit)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME versions26 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/version26_check.py
                                         $<TARGET_FILE:trial-spawner>)
        add_test(NAME climate263 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/climate263_check.py
                                         $<TARGET_FILE:trial-climate-check>)
        add_test(NAME historical-results
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_results.py
                         $<TARGET_FILE:trial-spawner>)
        add_test(NAME historical-consistency COMMAND ${Python3_EXECUTABLE}
                                                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_history.py)
        add_test(NAME ranking COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ranking.py
                                      $<TARGET_FILE:trial-spawner>)
        add_test(NAME protocol COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol.py
                                       $<TARGET_FILE:trial-spawner>)
    endif()
endif()
