# Opt-in native Vulkan build of AMD FidelityFX SDK v1.1.4 sourced from local SDK.
# This is intentionally not enabled in the regular Lichii build.
if(NOT LICHII_AMD_FFX3_NATIVE)
    return()
endif()

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "Native AMD FSR 3 SDK experiment is for Linux only")
endif()

set(FFX_AMD_ROOT "${CMAKE_SOURCE_DIR}/externals/amd-fidelityfx-sdk114/sdk")
if(NOT EXISTS "${FFX_AMD_ROOT}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp")
    message(FATAL_ERROR "Missing local AMD SDK copy. Run the bridge installer first.")
endif()

# All SPIR-V permutation headers are generated already from AMD v1.1.4
# Vulkan shader source. We validate their SHA-256s in native.py, then stage them
# at the original AMD CMake include path. Neither Wine nor a Windows executable
# runs in the native Bazzite build or in the resulting AppImage.
set(FFX_PERM_SOURCE "${CMAKE_SOURCE_DIR}/externals/amd-fsr3-shader-permutations-114/include")
if(NOT EXISTS "${FFX_PERM_SOURCE}/ffx_fsr3upscaler_accumulate_pass_permutations.h")
    message(FATAL_ERROR "Pre-generated FSR 3.1.4 shader headers missing. Run native.py apply.")
endif()
file(GLOB _ffx_native_headers LIST_DIRECTORIES FALSE "${FFX_PERM_SOURCE}/*.h")
list(LENGTH _ffx_native_headers _ffx_native_count)
if(NOT _ffx_native_count EQUAL 364)
    message(FATAL_ERROR "FSR 3.1.4 native shader headers incomplete: ${_ffx_native_count}/364")
endif()
set(FFX_PASS_PREBUILT_DIR "${CMAKE_BINARY_DIR}/amd-fidelityfx-sdk114/src/backends/shaders/vk")
file(MAKE_DIRECTORY "${FFX_PASS_PREBUILT_DIR}")
file(COPY ${_ffx_native_headers} DESTINATION "${FFX_PASS_PREBUILT_DIR}")

set(FFX_API_BACKEND VK_X64)
set(FFX_API_VK ON)
set(FFX_BUILD_AS_DLL OFF)
set(FFX_AUTO_COMPILE_SHADERS OFF CACHE BOOL "Use verified prebuilt FSR 3.1.4 SPIR-V headers on native Linux" FORCE)
set(FFX_FSR3 ON)
set(FFX_FSR3UPSCALER ON)
set(FFX_FI ON)
set(FFX_OF ON)
set(FFX_ALL OFF)
message(STATUS "AMD FSR 3.1.4 official SDK: native Linux Vulkan build")
message(STATUS "Validated prebuilt AMD shader permutations: ${_ffx_native_count} headers (NO Wine)")
add_subdirectory("${FFX_AMD_ROOT}" "${CMAKE_BINARY_DIR}/amd-fidelityfx-sdk114")

target_sources(video_core PRIVATE
    renderer_vulkan/vk_amd_fsr_sdk_upscaler.cpp
    renderer_vulkan/vk_amd_fsr_sdk_upscaler.h
    # LICHII_AMD_UNIVERSAL_OPTICAL_FLOW_V1
    renderer_vulkan/present/amd_universal_optical_flow.cpp
    renderer_vulkan/present/amd_universal_optical_flow.h)
target_include_directories(video_core PRIVATE "${FFX_AMD_ROOT}/include")
target_compile_definitions(video_core PRIVATE LICHII_AMD_FFX_NATIVE_114=1)
target_link_libraries(video_core PRIVATE
    ffx_backend_vk_x64
    ffx_fsr3upscaler_x64
    ffx_fsr3_x64
    ffx_opticalflow_x64
    ffx_frameinterpolation_x64
    Vulkan::Vulkan)
