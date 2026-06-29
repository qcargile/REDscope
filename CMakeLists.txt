cmake_minimum_required(VERSION 3.21)

# vcpkg triplet (static spdlog/fmt, dynamic CRT). Must precede project().
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
endif()

project(REDscope VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "REDscope is Windows-only.")
endif()

include(FetchContent)
FetchContent_Declare(
    RED4extSDK
    GIT_REPOSITORY https://github.com/WopsS/RED4ext.SDK.git
    GIT_TAG 1fd4cef1ca91bf8d38125e2c370f1c8211afed77   # master as of 2026-04-17
)
FetchContent_MakeAvailable(RED4extSDK)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
)
FetchContent_MakeAvailable(miniz)

find_package(spdlog CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)

add_subdirectory(cleanslate)

add_library(REDscope SHARED
    src/Plugin.cpp
    src/Logger.cpp
    src/Config.cpp
    src/breadcrumbs/BreadcrumbApi.cpp
    src/breadcrumbs/CrashArchive.cpp
    src/hooks/Hooks.cpp
    src/hooks/DispatchHook.cpp
    src/hooks/ScriptStack.cpp
    src/logs/LogTailer.cpp
    src/modules/ModuleEnumerator.cpp
    src/util/SehGuardedRead.cpp
    src/util/PreallocatedBuffer.cpp
    src/handler/SehHandler.cpp
    src/handler/NestedFallback.cpp
    src/handler/HangWatchdog.cpp
    src/report/CrashClass.cpp
    src/report/CulpritHeuristic.cpp
    src/report/Fingerprint.cpp
    src/report/JsonSidecar.cpp
    src/report/ReportCheckpoint.cpp
    src/report/Sections.cpp
    src/report/SetupSections.cpp
    src/report/StackDump.cpp
    src/rtti/FaultInstruction.cpp
    src/rtti/HandleDetect.cpp
    src/rtti/HashDecoder.cpp
    src/rtti/HashDecoderSdk.cpp
    src/rtti/ObjectsInFlight.cpp
    src/rtti/PointerType.cpp
    src/snapshot/CuratedConflicts.cpp
    src/snapshot/EngineDumpEnricher.cpp
    src/snapshot/EngineStateLive.cpp
    src/snapshot/GameStateLive.cpp
    src/snapshot/GpuState.cpp
    src/snapshot/HardwareSpecs.cpp
    src/snapshot/InstalledMods.cpp
    src/snapshot/PluginMetadata.cpp
    src/snapshot/RttiSnapshot.cpp
    src/snapshot/SessionHistory.cpp
    src/snapshot/SetupIntegrity.cpp
    src/snapshot/SnapshotWorker.cpp
    src/snapshot/WrapChain.cpp
    src/symbols/SymbolDb.cpp
    src/symbols/SymbolFetch.cpp
)

target_include_directories(REDscope PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${miniz_SOURCE_DIR}
)

target_link_libraries(REDscope PRIVATE
    RED4ext.SDK
    cleancore
    spdlog::spdlog
    simdjson::simdjson
    dbghelp
    psapi
    version
    dxgi
    advapi32
    PowrProf
    User32
    shell32
    winhttp
    miniz
)

target_compile_definitions(REDscope PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    REDSCOPE_USE_RTTI
    REDSCOPE_VERSION="0.6.0"
)

target_compile_options(REDscope PRIVATE /Zi)
target_link_options(REDscope PRIVATE /DEBUG /OPT:REF /OPT:ICF)

# Static libs + dynamic CRT (keeps spdlog.dll/fmt.dll out of the game's DLL search path).
set_property(TARGET REDscope PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Deployment: REDscope is an MO2-managed mod. NEVER write to the Steam install.
set(REDSCOPE_MOD_ROOT      "${CMAKE_SOURCE_DIR}" CACHE PATH "Mod folder root (defaults to the source tree)")
set(REDSCOPE_PLUGIN_DEPLOY "${REDSCOPE_MOD_ROOT}/red4ext/plugins/REDscope")
set(REDSCOPE_CET_DIR       "${REDSCOPE_MOD_ROOT}/bin/x64/plugins/cyber_engine_tweaks/mods/REDscope")
add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSCOPE_PLUGIN_DEPLOY}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:REDscope> "${REDSCOPE_PLUGIN_DEPLOY}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:REDscope> "${REDSCOPE_PLUGIN_DEPLOY}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/REDscope.ini" "${REDSCOPE_PLUGIN_DEPLOY}/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSCOPE_PLUGIN_DEPLOY}/conflicts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/conflicts/README.md" "${REDSCOPE_PLUGIN_DEPLOY}/conflicts/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSCOPE_CET_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/bin/x64/plugins/cyber_engine_tweaks/mods/REDscope/init.lua" "${REDSCOPE_CET_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSCOPE_CET_DIR}/analyzer/core" "${REDSCOPE_CET_DIR}/analyzer/rules" "${REDSCOPE_CET_DIR}/analyzer/conformance"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/core/analyzer.lua" "${REDSCOPE_CET_DIR}/analyzer/core/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/rules/engine-signatures.json" "${REDSCOPE_CET_DIR}/analyzer/rules/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/rules/mod-attribution.json" "${REDSCOPE_CET_DIR}/analyzer/rules/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/conformance/cases.json" "${REDSCOPE_CET_DIR}/analyzer/conformance/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/conformance/test-rules.json" "${REDSCOPE_CET_DIR}/analyzer/conformance/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/core/grouping.lua" "${REDSCOPE_CET_DIR}/analyzer/core/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/core/crashReportParser.lua" "${REDSCOPE_CET_DIR}/analyzer/core/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/analyzer/conformance/grouping-cases.json" "${REDSCOPE_CET_DIR}/analyzer/conformance/"
    DEPENDS REDscope
)
add_custom_target(deploy-and-sync
    COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/../sync-mods.ps1"
    DEPENDS deploy
)

enable_testing()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
