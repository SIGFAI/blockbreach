set(TARGET RoNPassthrough)

add_library(${TARGET} SHARED
    dllmain.cpp
    compositor.cpp
    crash_guard.cpp
    ws.cpp
)

target_include_directories(${TARGET} PRIVATE . "${CMAKE_CURRENT_SOURCE_DIR}/../reshade/include")
target_compile_definitions(${TARGET} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE ws2_32 d3d11)
