set(P3P_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "p3p: profile configured (${P3P_PROFILE_DIR})")

add_library(p3p_host_stub STATIC
    "${P3P_PROFILE_DIR}/host/p3p_profile.cpp"
)
target_include_directories(p3p_host_stub PUBLIC
    "${P3P_PROFILE_DIR}/host"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(p3p_host_stub PUBLIC psprecomp_core)

add_executable(p3p_trace_runner
    "${P3P_PROFILE_DIR}/tools/p3p_trace_runner.cpp"
)
target_include_directories(p3p_trace_runner PUBLIC
    "${P3P_PROFILE_DIR}/host"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(p3p_trace_runner PUBLIC psprecomp_core)
