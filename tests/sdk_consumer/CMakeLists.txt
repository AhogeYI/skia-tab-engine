cmake_minimum_required(VERSION 3.24)
project(TabEngineSdkConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
find_package(TabEngine CONFIG REQUIRED)
add_executable(tabengine_sdk_consumer main.cpp)
target_link_libraries(tabengine_sdk_consumer PRIVATE TabEngine::SDK TabEngine::FreeType)
