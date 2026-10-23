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

# Kernel + ThreadMan HLE (Gate 4B scope).
add_library(p3p_kernel STATIC
    "${P3P_PROFILE_DIR}/host/kernel/p3p_kernel.cpp"
)
target_include_directories(p3p_kernel PUBLIC
    "${P3P_PROFILE_DIR}/host"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(p3p_kernel PUBLIC psprecomp_core)

add_library(p3p_hle STATIC
    "${P3P_PROFILE_DIR}/host/hle/p3p_hle_threadman.cpp"
    "${P3P_PROFILE_DIR}/host/hle/p3p_hle_io.cpp"
    "${P3P_PROFILE_DIR}/host/hle/p3p_hle_sysmem.cpp"
    "${P3P_PROFILE_DIR}/host/hle/p3p_hle_gedisplay.cpp"
    "${P3P_PROFILE_DIR}/host/hle/p3p_hle_audio.cpp"
    "${P3P_PROFILE_DIR}/host/hle/p3p_hle_umd.cpp"
    "${P3P_PROFILE_DIR}/host/hle/p3p_hle_services.cpp"
)
target_include_directories(p3p_hle PUBLIC
    "${P3P_PROFILE_DIR}/host"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(p3p_hle PUBLIC p3p_kernel psprecomp_core)

target_link_libraries(p3p_host_stub PUBLIC p3p_hle p3p_kernel)

# AOT executable host (links the generated corpus when present).
add_executable(p3p
    "${P3P_PROFILE_DIR}/host/main.cpp"
)
target_include_directories(p3p PRIVATE
    "${P3P_PROFILE_DIR}/host"
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(p3p PRIVATE p3p_host_stub p3p_kernel p3p_hle psprecomp_core)
if(P3P_GENERATED)
    target_link_libraries(p3p PRIVATE p3p_generated)
    target_compile_definitions(p3p PRIVATE P3P_HAS_GENERATED=1)
endif()

if(PSPRECOMP_BUILD_PROFILE_TESTS)
    add_executable(test_p3p_threading
        "${P3P_PROFILE_DIR}/tests/test_p3p_threading.cpp"
    )
    target_include_directories(test_p3p_threading PRIVATE
        "${P3P_PROFILE_DIR}/host"
        "${CMAKE_SOURCE_DIR}/include"
    )
    target_link_libraries(test_p3p_threading PRIVATE p3p_kernel p3p_hle psprecomp_core)
    add_test(NAME test_p3p_threading COMMAND test_p3p_threading)

    add_executable(test_p3p_io
        "${P3P_PROFILE_DIR}/tests/test_p3p_io.cpp"
    )
    target_include_directories(test_p3p_io PRIVATE
        "${P3P_PROFILE_DIR}/host"
        "${CMAKE_SOURCE_DIR}/include"
    )
    target_link_libraries(test_p3p_io PRIVATE p3p_kernel p3p_hle psprecomp_core)
    add_test(NAME test_p3p_io COMMAND test_p3p_io)

    add_executable(test_p3p_services
        "${P3P_PROFILE_DIR}/tests/test_p3p_services.cpp"
    )
    target_include_directories(test_p3p_services PRIVATE
        "${P3P_PROFILE_DIR}/host"
        "${CMAKE_SOURCE_DIR}/include"
    )
    target_link_libraries(test_p3p_services PRIVATE p3p_kernel p3p_hle psprecomp_core)
    add_test(NAME test_p3p_services COMMAND test_p3p_services)

    add_executable(test_p3p_yield
        "${P3P_PROFILE_DIR}/tests/test_p3p_yield.cpp"
    )
    target_include_directories(test_p3p_yield PRIVATE
        "${P3P_PROFILE_DIR}/host"
        "${CMAKE_SOURCE_DIR}/include"
    )
    target_link_libraries(test_p3p_yield PRIVATE p3p_kernel p3p_hle psprecomp_core)
    add_test(NAME test_p3p_yield COMMAND test_p3p_yield)
endif()
