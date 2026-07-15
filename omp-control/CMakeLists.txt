cmake_minimum_required(VERSION 3.19)
project(sampvoice_omp_control LANGUAGES CXX)
enable_testing()

set(OPENMP_SDK_ROOT "" CACHE PATH "Path to the open.mp SDK checkout")
set(OPENMP_NETWORK_INCLUDE "" CACHE PATH "Directory containing bitstream.hpp from open.mp-network")
option(SAMPVOICE_OMP_BUILD_COMPONENT "Build the SDK-dependent open.mp component" OFF)

if(SAMPVOICE_OMP_BUILD_COMPONENT)
    if(NOT EXISTS "${OPENMP_SDK_ROOT}/include/sdk.hpp")
        message(FATAL_ERROR "Set OPENMP_SDK_ROOT to an open.mp SDK checkout.")
    endif()
    if(NOT EXISTS "${OPENMP_NETWORK_INCLUDE}/bitstream.hpp")
        message(FATAL_ERROR "Set OPENMP_NETWORK_INCLUDE to the open.mp-network include directory.")
    endif()

    function(configure_omp_component target)
        target_compile_features(${target} PRIVATE cxx_std_17)
        target_compile_definitions(${target} PRIVATE
            OMP_VERSION_MAJOR=1
            OMP_VERSION_MINOR=5
            OMP_VERSION_PATCH=8
            GLM_FORCE_QUAT_DATA_WXYZ
            GLM_FORCE_SSE2
            NOMINMAX
            nssv_CONFIG_SELECT_STRING_VIEW=nssv_STRING_VIEW_NONSTD
            span_CONFIG_SELECT_SPAN=span_SPAN_NONSTD
            _GLIBCXX_USE_CXX11_ABI=0
        )
        target_compile_options(${target} PRIVATE
            -msse2
            -mfpmath=sse
            -fno-eliminate-unused-debug-types
            -fno-sized-deallocation
        )
        target_include_directories(${target} PRIVATE
        include
        "${OPENMP_SDK_ROOT}/include"
        "${OPENMP_SDK_ROOT}/lib/glm"
        "${OPENMP_SDK_ROOT}/lib/span-lite/include"
        "${OPENMP_SDK_ROOT}/lib/string-view-lite/include"
        "${OPENMP_SDK_ROOT}/lib/robin-hood-hashing/src/include"
        "${OPENMP_NETWORK_INCLUDE}"
        )
    endfunction()

    add_library(sampvoice-omp-control SHARED src/component.cpp)
    configure_omp_component(sampvoice-omp-control)
    set_target_properties(sampvoice-omp-control PROPERTIES PREFIX "" OUTPUT_NAME "sampvoice-omp-control")

    add_library(sampvoice-omp-probe SHARED src/probe.cpp)
    configure_omp_component(sampvoice-omp-probe)
    set_target_properties(sampvoice-omp-probe PROPERTIES PREFIX "" OUTPUT_NAME "sampvoice-omp-probe")
endif()

add_executable(protocol-test tests/protocol_test.cpp)
target_compile_features(protocol-test PRIVATE cxx_std_17)
target_include_directories(protocol-test PRIVATE include)
add_test(NAME protocol-test COMMAND protocol-test)
