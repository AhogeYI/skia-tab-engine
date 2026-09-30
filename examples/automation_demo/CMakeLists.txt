# The packaged copy of this example builds against an extracted SDK only:
# it declares no source-tree dependency and links the public targets.
cmake_minimum_required(VERSION 3.24)
project(TabEngineAutomationDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TABENGINE_SDK_ROOT "" CACHE PATH "Extracted TabEngine SDK root")
if(NOT TABENGINE_SDK_ROOT)
    message(FATAL_ERROR "Set TABENGINE_SDK_ROOT to an extracted TabEngine SDK root")
endif()

find_package(TabEngine 0.1.0 EXACT CONFIG REQUIRED
    PATHS "${TABENGINE_SDK_ROOT}" NO_DEFAULT_PATH)

add_executable(tabengine_automation_demo main.cpp)
target_compile_definitions(tabengine_automation_demo PRIVATE UNICODE _UNICODE)
target_link_libraries(tabengine_automation_demo PRIVATE
    TabEngine::SDK
    TabEngine::Automation
    TabEngine::AutomationShell
    TabEngine::AutomationTransport)
add_custom_command(TARGET tabengine_automation_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TABENGINE_SDK_ROOT}/bin/skia.dll"
        "$<TARGET_FILE_DIR:tabengine_automation_demo>/skia.dll")
