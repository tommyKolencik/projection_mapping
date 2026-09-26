cmake_minimum_required(VERSION 3.16)

project(projection_mapping VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.71 CONFIG REQUIRED)
find_package(nlohmann_json 3.7 REQUIRED)
find_package(Threads REQUIRED)

add_executable(projection_backend
    backend/src/main.cpp
)

target_link_libraries(projection_backend PRIVATE
    Boost::headers
    nlohmann_json::nlohmann_json
    Threads::Threads
)

target_compile_options(projection_backend PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_definitions(projection_backend PRIVATE
    FRONTEND_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/frontend"
)