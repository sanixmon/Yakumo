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

# Generated AOT corpus (profiles/p3p/generated, produced by psp_recomp --auto).
file(GLOB P3P_GENERATED CONFIGURE_DEPENDS "${P3P_PROFILE_DIR}/generated/*.cpp")
if(P3P_GENERATED)
    list(LENGTH P3P_GENERATED P3P_GENERATED_COUNT)
    message(STATUS "p3p: ${P3P_GENERATED_COUNT} generated AOT units")
    add_library(p3p_generated OBJECT ${P3P_GENERATED})
    target_include_directories(p3p_generated PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${P3P_PROFILE_DIR}/generated")
    set_target_properties(p3p_generated PROPERTIES JOB_POOL_COMPILE psprecomp_generated)
else()
    message(STATUS "p3p: no generated AOT units; run psp_recomp --auto first")
endif()
