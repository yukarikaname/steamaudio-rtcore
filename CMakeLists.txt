cmake_minimum_required(VERSION 3.25)

project(steamaudio_rtcore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

option(RTCORE_BUILD_METAL "Build the Metal 4 backend (Apple only)." ON)
option(RTCORE_BUILD_VULKAN "Build the Vulkan ray-query backend." ON)
option(RTCORE_ENABLE_STEAMAUDIO "Build the Steam Audio custom-tracer adapter." ON)
option(RTCORE_BUILD_TESTS "Build tests." ON)
option(RTCORE_BUILD_BENCH "Build the benchmark tool." ON)

set(RTCORE_SOURCES
    src/status.cpp
)

add_library(rtcore STATIC ${RTCORE_SOURCES})
target_include_directories(rtcore
    PUBLIC "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(rtcore PRIVATE /W4)
else()
    target_compile_options(rtcore PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# ------------------------------------------------------------------------------
# Vulkan backend
# ------------------------------------------------------------------------------

if(RTCORE_BUILD_VULKAN)
    find_package(Vulkan QUIET)
    find_program(RTCORE_GLSLANG_VALIDATOR
        NAMES glslangValidator glslang
        HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")

    if(Vulkan_FOUND AND RTCORE_GLSLANG_VALIDATOR)
        set(RTCORE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
        set(RTCORE_VULKAN_SHADER "${CMAKE_SOURCE_DIR}/src/shaders/vulkan/ray_query.comp")
        set(RTCORE_VULKAN_SPV "${RTCORE_GENERATED_DIR}/ray_query.spv")
        set(RTCORE_VULKAN_HEADER "${RTCORE_GENERATED_DIR}/vulkan_ray_query_spv.h")

        add_custom_command(
            OUTPUT "${RTCORE_VULKAN_SPV}" "${RTCORE_VULKAN_HEADER}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${RTCORE_GENERATED_DIR}"
            COMMAND "${RTCORE_GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
                    -o "${RTCORE_VULKAN_SPV}" "${RTCORE_VULKAN_SHADER}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${RTCORE_VULKAN_SPV}"
                    "-DOUTPUT=${RTCORE_VULKAN_HEADER}" -DNAME=kRayQuerySpv
                    -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
            DEPENDS "${RTCORE_VULKAN_SHADER}" "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
            COMMENT "Compiling Vulkan ray-query shader"
            VERBATIM)

        target_sources(rtcore PRIVATE
            src/backend/vulkan/vulkan_tracer.cpp
            src/backend/vulkan/vulkan_device.cpp
            src/backend/vulkan/vulkan_tracer_internal.h
            "${RTCORE_VULKAN_HEADER}")
        target_include_directories(rtcore PRIVATE "${RTCORE_GENERATED_DIR}")
        target_link_libraries(rtcore PUBLIC Vulkan::Vulkan)
        target_compile_definitions(rtcore PUBLIC RTCORE_HAS_VULKAN=1)
        message(STATUS "Vulkan ray-query backend enabled")
    else()
        target_sources(rtcore PRIVATE src/backend/vulkan/vulkan_tracer_stub.cpp)
        target_compile_definitions(rtcore PUBLIC RTCORE_HAS_VULKAN=0)
        message(STATUS "Vulkan backend disabled (Vulkan SDK or glslangValidator missing)")
    endif()
else()
    target_sources(rtcore PRIVATE src/backend/vulkan/vulkan_tracer_stub.cpp)
    target_compile_definitions(rtcore PUBLIC RTCORE_HAS_VULKAN=0)
endif()

# ------------------------------------------------------------------------------
# Metal 4 backend
# ------------------------------------------------------------------------------

if(APPLE AND RTCORE_BUILD_METAL)
    target_sources(rtcore PRIVATE
        src/backend/metal/metal_tracer.mm
        src/backend/metal/ray_query_msl.h)
    set_source_files_properties(src/backend/metal/metal_tracer.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rtcore PUBLIC "-framework Metal" "-framework Foundation")
    target_compile_definitions(rtcore PUBLIC RTCORE_HAS_METAL=1)
    message(STATUS "Metal 4 backend enabled")
else()
    target_sources(rtcore PRIVATE src/backend/metal/metal_tracer_stub.cpp)
    target_compile_definitions(rtcore PUBLIC RTCORE_HAS_METAL=0)
endif()

# ------------------------------------------------------------------------------
# Steam Audio adapter (upstream is built as an isolated external project)
# ------------------------------------------------------------------------------

set(STEAMAUDIO_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/steam-audio/core")
set(STEAMAUDIO_BINARY_DIR "${CMAKE_BINARY_DIR}/steam-audio")
set(STEAMAUDIO_DEP_OVERRIDES "")
if(APPLE)
    list(APPEND STEAMAUDIO_DEP_OVERRIDES
        "-DMySOFA_LIBRARY=${STEAMAUDIO_SOURCE_DIR}/deps/mysofa/lib/osx/release/libmysofa.dylib"
        "-DMySOFA_LIBRARY_DEBUG=${STEAMAUDIO_SOURCE_DIR}/deps/mysofa/lib/osx/release/libmysofa.dylib"
        "-DMySOFA_INCLUDE_DIR=${STEAMAUDIO_SOURCE_DIR}/deps/mysofa/include")
endif()

set(STEAMAUDIO_DEPS_PRESENT
    EXISTS "${STEAMAUDIO_SOURCE_DIR}/deps/flatbuffers"
    AND EXISTS "${STEAMAUDIO_SOURCE_DIR}/deps/pffft"
    AND EXISTS "${STEAMAUDIO_SOURCE_DIR}/deps/mysofa")

if(RTCORE_ENABLE_STEAMAUDIO AND STEAMAUDIO_DEPS_PRESENT)
    include(ExternalProject)

    ExternalProject_Add(steamaudio_project
        SOURCE_DIR "${STEAMAUDIO_SOURCE_DIR}"
        BINARY_DIR "${STEAMAUDIO_BINARY_DIR}"
        INSTALL_DIR "${STEAMAUDIO_BINARY_DIR}/install"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${STEAMAUDIO_BINARY_DIR}/install
            -DBUILD_SHARED_LIBS=ON
            -DSTEAMAUDIO_BUILD_TESTS=OFF
            -DSTEAMAUDIO_BUILD_BENCHMARKS=OFF
            -DSTEAMAUDIO_BUILD_SAMPLES=OFF
            -DSTEAMAUDIO_BUILD_ITESTS=OFF
            -DSTEAMAUDIO_BUILD_DOCS=OFF
            -DSTEAMAUDIO_ENABLE_EMBREE=OFF
            -DSTEAMAUDIO_ENABLE_IPP=OFF
            -DSTEAMAUDIO_ENABLE_MKL=OFF
            -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${STEAMAUDIO_BINARY_DIR}/lib
            ${STEAMAUDIO_DEP_OVERRIDES}

            -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${STEAMAUDIO_BINARY_DIR}/bin
        BUILD_COMMAND "${CMAKE_COMMAND}" --build "${STEAMAUDIO_BINARY_DIR}" --target phonon
        BUILD_BYPRODUCTS
            "${STEAMAUDIO_BINARY_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}phonon${CMAKE_SHARED_LIBRARY_SUFFIX}"
        INSTALL_COMMAND ""
        UPDATE_COMMAND "")

    add_library(steamaudio_phonon SHARED IMPORTED GLOBAL)
    file(MAKE_DIRECTORY "${STEAMAUDIO_BINARY_DIR}/src/core")
    set_target_properties(steamaudio_phonon PROPERTIES
        IMPORTED_LOCATION "${STEAMAUDIO_BINARY_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}phonon${CMAKE_SHARED_LIBRARY_SUFFIX}"
        INTERFACE_INCLUDE_DIRECTORIES "${STEAMAUDIO_SOURCE_DIR}/src/core;${STEAMAUDIO_BINARY_DIR}/src/core")

    add_library(steamaudio_dependency INTERFACE)
    target_link_libraries(steamaudio_dependency INTERFACE steamaudio_phonon)
    add_dependencies(steamaudio_dependency steamaudio_project)

    target_sources(rtcore PRIVATE src/steam_audio_adapter.cpp)
    target_include_directories(rtcore PRIVATE src)
    target_link_libraries(rtcore PUBLIC steamaudio_dependency)
    target_compile_definitions(rtcore PUBLIC RTCORE_HAS_STEAMAUDIO=1)
    add_dependencies(rtcore steamaudio_dependency)
    set(RTCORE_HAS_STEAMAUDIO ON)
    message(STATUS "Steam Audio adapter enabled (pinned submodule)")
elseif(RTCORE_ENABLE_STEAMAUDIO)
    target_compile_definitions(rtcore PUBLIC RTCORE_HAS_STEAMAUDIO=0)
    set(RTCORE_HAS_STEAMAUDIO OFF)
    message(WARNING
        "Steam Audio dependencies are missing under external/steam-audio/core/deps. "
        "Run 'python3 get_dependencies.py -p osx -a arm64' from external/steam-audio/core/build, "
        "or configure with -DRTCORE_ENABLE_STEAMAUDIO=OFF.")
else()
    target_compile_definitions(rtcore PUBLIC RTCORE_HAS_STEAMAUDIO=0)
endif()

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------

set(RTCORE_RUNTIME_RPATH "${STEAMAUDIO_BINARY_DIR}/lib")
if(APPLE AND RTCORE_HAS_STEAMAUDIO)
    list(APPEND RTCORE_RUNTIME_RPATH
        "${STEAMAUDIO_SOURCE_DIR}/deps/pffft/lib/osx/release"
        "${STEAMAUDIO_SOURCE_DIR}/deps/mysofa/lib/osx/release")
endif()

if(RTCORE_BUILD_TESTS)
    enable_testing()
    add_executable(rtcore_test_backend tests/test_backend.cpp)
    target_link_libraries(rtcore_test_backend PRIVATE rtcore)
    set_target_properties(rtcore_test_backend PROPERTIES BUILD_RPATH "${RTCORE_RUNTIME_RPATH}")
    add_test(NAME backend COMMAND rtcore_test_backend)
    set_tests_properties(backend PROPERTIES SKIP_RETURN_CODE 77)

    if(RTCORE_HAS_STEAMAUDIO)
        add_executable(rtcore_test_steamaudio tests/test_steamaudio.cpp)
        target_link_libraries(rtcore_test_steamaudio PRIVATE rtcore)
        set_target_properties(rtcore_test_steamaudio PROPERTIES BUILD_RPATH "${RTCORE_RUNTIME_RPATH}")
        add_test(NAME steamaudio COMMAND rtcore_test_steamaudio)
        set_tests_properties(steamaudio PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

if(RTCORE_BUILD_BENCH)
    add_executable(rtcore_bench bench/bench_tracer.cpp)
    target_link_libraries(rtcore_bench PRIVATE rtcore)
    set_target_properties(rtcore_bench PROPERTIES BUILD_RPATH "${RTCORE_RUNTIME_RPATH}")
endif()
