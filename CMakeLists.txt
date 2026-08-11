# ==============================================================================
# CMakeLists.txt  –  Project Synapse iGPU Shim
# Supports: MSVC 19.x (Windows 11) and Clang 17+ (Ubuntu 24.04 LTS)
# C++20, Vulkan Implicit Layer shared library + tests + tools
# ==============================================================================
cmake_minimum_required(VERSION 3.22)
project(SynapseIGPUShim VERSION 1.3.0 LANGUAGES CXX)

# ── Language standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # No compiler-specific extensions

# ── Compiler diagnostics ───────────────────────────────────────────────────────
if (MSVC)
    # /W4: most important warnings; /permissive-: strict mode
    # Suppress specific warnings that are safe to ignore in this codebase
    add_compile_options(/W4 /permissive- /utf-8 /EHsc
        /w44010 /w44324 /w44100 /w44189 /w44530 /w44996)
    # Do NOT treat warnings as errors during initial development
    # add_compile_options(/WX)
    # Silence MSVC STL warnings about C++17 deprecations in C++20 mode
    add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(
        -Wall -Wextra -Werror
        -fvisibility=hidden          # hide all symbols by default; exports use VKAPI_ATTR
        -fvisibility-inlines-hidden  # inline functions also hidden
    )
endif()

# ── Vulkan ─────────────────────────────────────────────────────────────────────
# FindVulkan works on both Windows (reads VULKAN_SDK env var / registry) and
# Linux (searches standard system paths and VK_SDK_PATH / VULKAN_SDK).
find_package(Vulkan REQUIRED)

# ── Optional feature flags ─────────────────────────────────────────────────────
option(SYNAPSE_REAL_FENCE
    "Enable real kernel-mode fence queries (requires DRM headers on Linux / DXGK on Windows)"
    OFF)
option(SYNAPSE_STUB_DMA
    "Use synchronous DMA stub (disable for production hardware builds)"
    ON)
option(SYNAPSE_REAL_DMA
    "Enable real KMD DMA transfers via drmIoctl (Linux) or D3DKMTRender (Windows). Requires SYNAPSE_STUB_DMA=OFF."
    OFF)
option(SYNAPSE_POWER_VERIFY
    "Enable post-session power-budget assertions (CI lab builds only — NOT for release)"
    OFF)

# ── Shared layer library ───────────────────────────────────────────────────────
# Collects every synapse translation unit.  layer_entry.cpp exports the two
# vkGetInstanceProcAddr / vkGetDeviceProcAddr symbols required by the loader.
file(GLOB SYNAPSE_CORE_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/synapse/*.cpp"
)

# layer_entry.cpp lives directly in synapse/
set(LAYER_ENTRY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/synapse/layer_entry.cpp")

add_library(SynapseLayer SHARED
    ${SYNAPSE_CORE_SRCS}
    ${LAYER_ENTRY_SRC}
)

target_include_directories(SynapseLayer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/synapse"
    "${Vulkan_INCLUDE_DIRS}"
)

target_link_libraries(SynapseLayer PRIVATE Vulkan::Vulkan)

# Propagate optional feature flags as preprocessor macros
if (SYNAPSE_REAL_FENCE)
    target_compile_definitions(SynapseLayer PUBLIC SYNAPSE_REAL_FENCE=1)
endif()
if (SYNAPSE_STUB_DMA)
    target_compile_definitions(SynapseLayer PUBLIC SYNAPSE_STUB_DMA=1)
endif()
if (SYNAPSE_REAL_DMA)
    target_compile_definitions(SynapseLayer PUBLIC SYNAPSE_REAL_DMA=1)
endif()
if (SYNAPSE_POWER_VERIFY)
    target_compile_definitions(SynapseLayer PUBLIC SYNAPSE_POWER_VERIFY=1)
endif()

# On Linux, the loader discovers implicit layers via the XDG paths; name the
# library without the version suffix so the JSON manifest path is stable.
if (UNIX AND NOT APPLE)
    set_target_properties(SynapseLayer PROPERTIES
        OUTPUT_NAME     "SynapseLayer"
        VERSION         ${PROJECT_VERSION}
        SOVERSION       1
        SUFFIX          ".so"
    )
endif()

# ── JSON layer manifest ────────────────────────────────────────────────────────
# Generate a platform-correct VkLayer_synapse.json next to the built library.
if (WIN32)
    set(SYNAPSE_LAYER_FILENAME "SynapseLayer.dll")
else()
    set(SYNAPSE_LAYER_FILENAME "libSynapseLayer.so")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_synapse.json.in"
    "${CMAKE_BINARY_DIR}/VkLayer_synapse.json"
    @ONLY
)

add_custom_command(TARGET SynapseLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/VkLayer_synapse.json"
        "$<TARGET_FILE_DIR:SynapseLayer>/VkLayer_synapse.json"
    COMMENT "Deploying VkLayer_synapse.json alongside the shared library"
)

# ── CTest targets ──────────────────────────────────────────────────────────────
enable_testing()

file(GLOB TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/synapse/tests/*.cpp")
file(GLOB TOOL_TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/synapse/tools/test_*.cpp")

foreach(TEST_SRC ${TEST_SRCS})
    get_filename_component(TEST_NAME "${TEST_SRC}" NAME_WE)

    # --- Normal test binary ---
    add_executable("${TEST_NAME}" "${TEST_SRC}")
    target_include_directories("${TEST_NAME}" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/synapse"
        "${Vulkan_INCLUDE_DIRS}"
    )
    target_link_libraries("${TEST_NAME}" PRIVATE SynapseLayer Vulkan::Vulkan)
    add_test(NAME "${TEST_NAME}" COMMAND "${TEST_NAME}")

    # --- SYNAPSE_POWER_VERIFY variant (T3-4) ---
    add_executable("${TEST_NAME}_pv" "${TEST_SRC}")
    target_include_directories("${TEST_NAME}_pv" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/synapse"
        "${Vulkan_INCLUDE_DIRS}"
    )
    target_compile_definitions("${TEST_NAME}_pv" PRIVATE SYNAPSE_POWER_VERIFY=1)
    target_link_libraries("${TEST_NAME}_pv" PRIVATE SynapseLayer Vulkan::Vulkan)
    add_test(NAME "${TEST_NAME}_power_verify" COMMAND "${TEST_NAME}_pv")
endforeach()

foreach(TEST_SRC ${TOOL_TEST_SRCS})
    get_filename_component(TEST_NAME "${TEST_SRC}" NAME_WE)

    if(NOT TEST_NAME)
        continue()
    endif()

    # schema_migration is a header-only protocol utility; tests belong
    # with the tool tests because they do not require Vulkan linkage.
    add_executable("${TEST_NAME}" "${TEST_SRC}")
    target_include_directories("${TEST_NAME}" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/synapse"
        "${Vulkan_INCLUDE_DIRS}"
    )
    target_link_libraries("${TEST_NAME}" PRIVATE SynapseLayer Vulkan::Vulkan)
    add_test(NAME "${TEST_NAME}" COMMAND "${TEST_NAME}")
endforeach()

# ── Tool executables ───────────────────────────────────────────────────────────
file(GLOB TOOL_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/synapse/tools/*.cpp")

foreach(TOOL_SRC ${TOOL_SRCS})
    get_filename_component(TOOL_NAME "${TOOL_SRC}" NAME_WE)

    # Skip tools that are already registered as tests to avoid duplicate targets.
    if(TOOL_NAME MATCHES "^test_")
        continue()
    endif()

    add_executable("${TOOL_NAME}" "${TOOL_SRC}")
    target_include_directories("${TOOL_NAME}" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/synapse"
        "${Vulkan_INCLUDE_DIRS}"
    )
    target_link_libraries("${TOOL_NAME}" PRIVATE SynapseLayer Vulkan::Vulkan)

    # Benchmark/helper tools are useful enough to run in CI.
    if(TOOL_NAME MATCHES "^bench_")
        add_test(NAME "${TOOL_NAME}" COMMAND "${TOOL_NAME}")
    endif()
endforeach()

# ── Install rules (optional, for system-wide layer deployment) ─────────────────
install(TARGETS SynapseLayer
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES "${CMAKE_BINARY_DIR}/VkLayer_synapse.json"
    DESTINATION share/vulkan/implicit_layer.d
)
