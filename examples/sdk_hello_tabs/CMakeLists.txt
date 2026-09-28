cmake_minimum_required(VERSION 3.24)
project(TabEngineHelloTabs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# When this example lives inside an extracted SDK, its root is two levels up.
# A copied example can use -DTABENGINE_SDK_ROOT=<extracted-sdk-root> instead.
get_filename_component(_bundled_sdk_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TABENGINE_SDK_ROOT "${_bundled_sdk_root}" CACHE PATH "Extracted TabEngine SDK root")
if(NOT EXISTS "${TABENGINE_SDK_ROOT}/manifest.json" OR
   NOT EXISTS "${TABENGINE_SDK_ROOT}/lib/cmake/TabEngine/TabEngineConfig.cmake")
    message(FATAL_ERROR "Set TABENGINE_SDK_ROOT to the extracted TabEngine SDK root")
endif()

file(READ "${TABENGINE_SDK_ROOT}/manifest.json" _sdk_manifest)
string(JSON _sdk_configuration ERROR_VARIABLE _sdk_error
    GET "${_sdk_manifest}" configuration)
if(_sdk_error OR NOT _sdk_configuration MATCHES "^(Debug|Release)$")
    message(FATAL_ERROR "The SDK manifest has no supported configuration")
endif()
string(JSON _sdk_version ERROR_VARIABLE _sdk_version_error
    GET "${_sdk_manifest}" version)
if(_sdk_version_error OR NOT _sdk_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "The SDK manifest has no valid package version")
endif()
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${_sdk_configuration}" CACHE STRING
        "Configurations supported by this TabEngine SDK" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "${_sdk_configuration}" CACHE STRING
        "Configuration supported by this TabEngine SDK" FORCE)
elseif(NOT CMAKE_BUILD_TYPE STREQUAL _sdk_configuration)
    message(FATAL_ERROR "A ${_sdk_configuration} SDK cannot build ${CMAKE_BUILD_TYPE}")
endif()

find_package(TabEngine ${_sdk_version} EXACT CONFIG REQUIRED
    PATHS "${TABENGINE_SDK_ROOT}" NO_DEFAULT_PATH)
add_executable(tabengine_hello_tabs WIN32 main.cpp)
target_compile_definitions(tabengine_hello_tabs PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(tabengine_hello_tabs PRIVATE TabEngine::SDK)
add_custom_command(TARGET tabengine_hello_tabs POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${TABENGINE_SDK_ROOT}/bin/skia.dll"
        "$<TARGET_FILE_DIR:tabengine_hello_tabs>/skia.dll"
    VERBATIM)
