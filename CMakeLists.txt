cmake_minimum_required(VERSION 3.24)
project(VANTIX VERSION 0.1.0 LANGUAGES CXX)

include(CheckLanguage)
check_language(CUDA)
option(VANTIX_ENABLE_CUDA "Build the optional NVIDIA CUDA backend module"
    ${CMAKE_CUDA_COMPILER})
if(WIN32)
    find_program(VANTIX_HIPCC NAMES hipcc.exe hipcc HINTS "$ENV{HIP_PATH}/bin")
    option(VANTIX_ENABLE_HIP "Build the optional AMD HIP backend module"
        ${VANTIX_HIPCC})
else()
    check_language(HIP)
    option(VANTIX_ENABLE_HIP "Build the optional AMD HIP backend module"
        ${CMAKE_HIP_COMPILER})
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(OpenSSL 3 REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)

add_library(vantix_core
    src/ton.cpp
    src/cpu_backend.cpp
    src/device_manager.cpp
    src/runtime.cpp
    src/config.cpp
    src/vulkan_backend.cpp
    src/resources.cpp
    src/tuning_cache.cpp
    src/vendor_backend.cpp
)
set_target_properties(vantix_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(vantix_core PUBLIC include PRIVATE ${VULKAN_INCLUDE_DIR})
target_compile_definitions(vantix_core PRIVATE VANTIX_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vantix_core PRIVATE OpenSSL::Crypto tomlplusplus::tomlplusplus volk::volk ${CMAKE_DL_LIBS})
if(WIN32)
    target_compile_definitions(vantix_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(vantix_core PRIVATE Crypt32)
endif()

add_executable(vantix src/main.cpp)
target_link_libraries(vantix PRIVATE vantix_core CLI11::CLI11)

if(VANTIX_ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(vantix_cuda MODULE gpu/vendor/cuda_backend.cu)
    set_target_properties(vantix_cuda PROPERTIES CUDA_STANDARD 20
        CUDA_STANDARD_REQUIRED ON OUTPUT_NAME vantix_cuda)
    target_link_libraries(vantix_cuda PRIVATE CUDA::cudart_static CUDA::cuda_driver)
endif()

if(VANTIX_ENABLE_HIP)
    if(WIN32)
        # AMD's Windows HIP SDK does not support CMake's HIP language.
        # Compile only the independent C ABI module with AMD's hipcc driver.
        find_program(VANTIX_HIPINFO NAMES hipInfo.exe hipInfo
            HINTS "$ENV{HIP_PATH}/bin")
        set(VANTIX_HIP_ARCH_FLAGS)
        if(VANTIX_HIPINFO)
            execute_process(COMMAND "${VANTIX_HIPINFO}"
                OUTPUT_VARIABLE VANTIX_HIPINFO_OUTPUT ERROR_QUIET)
            string(REGEX MATCHALL "gcnArchName:[ \t]*gfx[0-9A-Za-z]+"
                VANTIX_HIP_ARCH_LINES "${VANTIX_HIPINFO_OUTPUT}")
            foreach(VANTIX_HIP_ARCH_LINE IN LISTS VANTIX_HIP_ARCH_LINES)
                string(REGEX MATCH "gfx[0-9A-Za-z]+" VANTIX_HIP_ARCH
                    "${VANTIX_HIP_ARCH_LINE}")
                list(APPEND VANTIX_HIP_ARCH_FLAGS "--offload-arch=${VANTIX_HIP_ARCH}")
            endforeach()
            list(REMOVE_DUPLICATES VANTIX_HIP_ARCH_FLAGS)
        endif()
        if(VANTIX_HIP_ARCH_FLAGS)
            add_custom_target(vantix_hip ALL
                COMMAND "${VANTIX_HIPCC}" -x hip -std=c++20 -O3
                    ${VANTIX_HIP_ARCH_FLAGS} -shared
                    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vendor/hip_backend.hip"
                    -o "$<TARGET_FILE_DIR:vantix>/vantix_hip.dll"
                DEPENDS gpu/vendor/hip_backend.hip gpu/vendor/backend_impl.hpp
                    gpu/vendor/derive_kernel.hpp gpu/vulkan/sha512_constants.slang
                COMMAND_EXPAND_LISTS VERBATIM)
            add_dependencies(vantix_hip vantix)
        else()
            message(WARNING "HIP SDK found, but hipInfo found no compatible AMD GPU; HIP module skipped")
        endif()
    else()
        enable_language(HIP)
        find_package(hip CONFIG REQUIRED)
        add_library(vantix_hip MODULE gpu/vendor/hip_backend.hip)
        set_target_properties(vantix_hip PROPERTIES HIP_STANDARD 20
            HIP_STANDARD_REQUIRED ON OUTPUT_NAME vantix_hip)
        target_link_libraries(vantix_hip PRIVATE hip::host)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(vantix_tests tests/core_tests.cpp)
    target_link_libraries(vantix_tests PRIVATE vantix_core)
    add_test(NAME core COMMAND vantix_tests)
    add_executable(vantix_vendor_kernel_tests tests/vendor_kernel_tests.cpp)
    target_link_libraries(vantix_vendor_kernel_tests PRIVATE vantix_core OpenSSL::Crypto)
    add_test(NAME vendor_kernel_reference COMMAND vantix_vendor_kernel_tests)
    add_executable(vantix_vendor_module_mock_tests tests/vendor_module_mock_tests.cpp)
    target_link_libraries(vantix_vendor_module_mock_tests PRIVATE vantix_core)
    add_test(NAME vendor_module_mock COMMAND vantix_vendor_module_mock_tests)
endif()
