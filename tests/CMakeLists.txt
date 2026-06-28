include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(redscope_tests
    test_config.cpp
    test_ring.cpp
    test_breadcrumb_store.cpp
    test_crash_class.cpp
    test_culprit_heuristic.cpp
    test_objects_in_flight.cpp
    test_curated_conflicts.cpp
    test_setup_integrity.cpp
    test_resource_loader_snapshot.cpp
    test_setup_sections.cpp
    test_fingerprint.cpp
    test_json_sidecar.cpp
    test_engine_dump_enricher.cpp
    test_handle_detect.cpp
    test_hash_decoder.cpp
    test_heartbeat.cpp
    test_sentinel_label.cpp
    test_module_snapshot.cpp
    test_seh_guarded_read.cpp
    test_log_tailer.cpp
    test_symbol_db.cpp
    test_emit_stack.cpp
    test_installed_mods.cpp
    test_gpu_state.cpp
    test_hardware_specs.cpp
    test_plugin_metadata.cpp
    test_rtti_snapshot.cpp
    test_script_stack.cpp
    test_session_history.cpp
    test_wrap_chain.cpp
    test_crash_archive.cpp
    test_symbol_resolve.cpp
    test_hang_watchdog.cpp
    test_rsdb_resolver.cpp
    test_resource_at_fault.cpp
    test_fault_instruction.cpp
    test_clean_core.cpp
    stubs/snap_stub.cpp
    ../src/Config.cpp
    ../src/breadcrumbs/BreadcrumbStore.cpp
    ../src/breadcrumbs/CrashArchive.cpp
    ../src/handler/HangWatchdog.cpp
    ../src/util/SehGuardedRead.cpp
    ../src/util/PreallocatedBuffer.cpp
    ../src/rtti/HandleDetect.cpp
    ../src/rtti/HashDecoder.cpp
    ../src/rtti/ObjectsInFlight.cpp
    ../src/rtti/FaultInstruction.cpp
    ../src/rtti/PointerType.cpp
    ../src/hooks/ScriptStack.cpp
    ../src/report/CrashClass.cpp
    ../src/report/CulpritHeuristic.cpp
    ../src/report/Fingerprint.cpp
    ../src/report/JsonSidecar.cpp
    ../src/report/ResourceAtFault.cpp
    ../src/report/ReportCheckpoint.cpp
    ../src/report/RsdbResolver.cpp
    ../src/report/SetupSections.cpp
    ../src/report/StackDump.cpp
    ../src/logs/LogTailer.cpp
    ../src/snapshot/CuratedConflicts.cpp
    ../src/snapshot/SetupIntegrity.cpp
    ../src/snapshot/ResourceLoaderSnapshot.cpp
    ../src/snapshot/GpuState.cpp
    ../src/snapshot/HardwareSpecs.cpp
    ../src/snapshot/EngineDumpEnricher.cpp
    ../src/snapshot/InstalledMods.cpp
    ../src/snapshot/PluginMetadata.cpp
    ../src/snapshot/RttiSnapshot.cpp
    ../src/snapshot/SessionHistory.cpp
    ../src/snapshot/WrapChain.cpp
    ../src/symbols/SymbolDb.cpp
    ../src/Logger.cpp
)
target_include_directories(redscope_tests PRIVATE ../src)
target_compile_definitions(redscope_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(redscope_tests PRIVATE /Zi)
target_link_options(redscope_tests PRIVATE /DEBUG)
target_link_libraries(redscope_tests PRIVATE GTest::gtest_main cleancore simdjson::simdjson spdlog::spdlog psapi dxgi advapi32 PowrProf User32 shell32 dbghelp)
include(GoogleTest)
gtest_discover_tests(redscope_tests)

find_program(REDSCOPE_NODE NAMES node)
if(REDSCOPE_NODE)
    add_test(NAME AnalyzerConformance
             COMMAND "${REDSCOPE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/../analyzer/conformance/run.mjs")
    add_test(NAME AnalyzerTwinParity
             COMMAND "${REDSCOPE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/../analyzer/conformance/twin-parity.mjs")
else()
    message(STATUS "node not found; AnalyzerConformance + AnalyzerTwinParity tests skipped")
endif()

# Perf harness - separate executable so its output isn't truncated by the
# test runner and so it can be run on demand rather than every CTest pass.
# Built from the same sources as the tests for the hot-path components it
# exercises (BreadcrumbStore + Ring).
add_executable(redscope_bench
    bench_perf.cpp
    ../src/Config.cpp
    ../src/breadcrumbs/BreadcrumbStore.cpp
)
target_include_directories(redscope_bench PRIVATE ../src)
target_compile_definitions(redscope_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(redscope_bench PRIVATE spdlog::spdlog)
