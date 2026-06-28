add_library(cleancore STATIC
    CleanCore.cpp
)

target_include_directories(cleancore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(cleancore PUBLIC
    simdjson::simdjson
    advapi32
)

target_compile_definitions(cleancore PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_property(TARGET cleancore PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
