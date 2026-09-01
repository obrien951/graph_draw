cmake_minimum_required(VERSION 3.16)
project(graph_draw VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Try Qt6 first, fall back to Qt5 (requires 5.14+ for QLineF::intersects)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_subdirectory(graph_lang)
add_subdirectory(graph_lang_cpp)
add_subdirectory(graph_lang_rust)
add_subdirectory(graph_core)
add_subdirectory(graph_io)
add_subdirectory(graph_analyze)
add_subdirectory(graph_merge)

enable_testing()
add_subdirectory(tests)

add_executable(graph_draw
    app/main.cpp
    app/mainwindow.cpp
    app/mainwindow.h
)

target_link_libraries(graph_draw PRIVATE
    graph_core
    graph_io
    Qt${QT_VERSION_MAJOR}::Widgets
)

# Command-line tool: scan a C++ repo and emit a dependency graph JSON.
add_executable(repo_to_graph
    analyze/main.cpp
)

target_link_libraries(repo_to_graph PRIVATE
    graph_analyze
    graph_core
    graph_io
    Qt${QT_VERSION_MAJOR}::Widgets
)

if(WIN32)
    set_target_properties(graph_draw PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(APPLE)
    set_target_properties(graph_draw PROPERTIES MACOSX_BUNDLE TRUE)
endif()
