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

    add_library(sampvoice-omp-control SHARED src/component.cpp)
    target_compile_features(sampvoice-omp-control PRIVATE cxx_std_17)
    target_include_directories(sampvoice-omp-control PRIVATE
        include
        "${OPENMP_SDK_ROOT}/include"
        "${OPENMP_NETWORK_INCLUDE}"
    )
    set_target_properties(sampvoice-omp-control PROPERTIES PREFIX "" OUTPUT_NAME "sampvoice-omp-control")
endif()

add_executable(protocol-test tests/protocol_test.cpp)
target_compile_features(protocol-test PRIVATE cxx_std_17)
target_include_directories(protocol-test PRIVATE include)
add_test(NAME protocol-test COMMAND protocol-test)
