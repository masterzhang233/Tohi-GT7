cmake_minimum_required(VERSION 3.19)
project(KytyDlssBackend LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Backend requires Windows x64 with MSVC or clang-cl")
endif()
set(KYTY_NGX_SDK_ROOT "" CACHE PATH "External NVIDIA DLSS SDK checkout")
if(NOT EXISTS "${KYTY_NGX_SDK_ROOT}/include/nvsdk_ngx_vk.h" OR
   NOT EXISTS "${KYTY_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
    message(FATAL_ERROR "Set KYTY_NGX_SDK_ROOT to the external SDK; no SDK files are vendored")
endif()
add_library(kyty_dlss_backend SHARED backend.cpp)
target_compile_features(kyty_dlss_backend PRIVATE cxx_std_20)
set_property(TARGET kyty_dlss_backend PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(kyty_dlss_backend PRIVATE VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN KYTY_DLSS_BACKEND_BUILD)
target_compile_options(kyty_dlss_backend PRIVATE /W4 /utf-8 /EHsc)
target_include_directories(kyty_dlss_backend SYSTEM PRIVATE "${KYTY_NGX_SDK_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/Vulkan-Headers/include")
target_link_libraries(kyty_dlss_backend PRIVATE
    "$<IF:$<CONFIG:Debug>,${KYTY_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib,${KYTY_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib>"
    advapi32 shell32 shlwapi ole32 crypt32 wintrust version)

# This regression test uses SDK result definitions only: no GPU or NGX library.
add_executable(kyty_dlss_feature_support_tests feature_support_tests.cpp)
target_compile_features(kyty_dlss_feature_support_tests PRIVATE cxx_std_20)
target_compile_options(kyty_dlss_feature_support_tests PRIVATE /W4 /WX /utf-8)
target_include_directories(kyty_dlss_feature_support_tests SYSTEM PRIVATE "${KYTY_NGX_SDK_ROOT}/include")
enable_testing()
add_test(NAME dlss_feature_support_lifecycle COMMAND kyty_dlss_feature_support_tests)

add_executable(kyty_dlss_backend_smoke smoke.cpp)
target_compile_features(kyty_dlss_backend_smoke PRIVATE cxx_std_20)
set_property(TARGET kyty_dlss_backend_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_definitions(kyty_dlss_backend_smoke PRIVATE VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(kyty_dlss_backend_smoke PRIVATE /W4 /utf-8 /EHsc)
target_include_directories(kyty_dlss_backend_smoke SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/Vulkan-Headers/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/nlohmann_json/single_include")

add_executable(kyty_dlss_device_probe device_probe.cpp)
target_compile_features(kyty_dlss_device_probe PRIVATE cxx_std_20)
set_property(TARGET kyty_dlss_device_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_definitions(kyty_dlss_device_probe PRIVATE VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(kyty_dlss_device_probe PRIVATE /W4 /utf-8 /EHsc)
target_link_libraries(kyty_dlss_device_probe PRIVATE onecore)
target_include_directories(kyty_dlss_device_probe SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/Vulkan-Headers/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/nlohmann_json/single_include")

# Diagnostic counterparts for the low default and explicit high Windows image base.
foreach(probe_target kyty_dlss_device_probe_fixedbase kyty_dlss_device_probe_highbase)
    add_executable(${probe_target} EXCLUDE_FROM_ALL device_probe.cpp)
    target_compile_features(${probe_target} PRIVATE cxx_std_20)
    set_property(TARGET ${probe_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_definitions(${probe_target} PRIVATE VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${probe_target} PRIVATE /W4 /utf-8 /EHsc)
    target_link_options(${probe_target} PRIVATE /DYNAMICBASE:NO)
    target_link_libraries(${probe_target} PRIVATE onecore)
    target_include_directories(${probe_target} SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/Vulkan-Headers/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/nlohmann_json/single_include")
endforeach()
target_link_options(kyty_dlss_device_probe_fixedbase PRIVATE /BASE:0x140000000)
target_link_options(kyty_dlss_device_probe_highbase PRIVATE /BASE:0x100000000000)
