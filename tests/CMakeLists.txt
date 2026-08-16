include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.0
)
FetchContent_MakeAvailable(Catch2)

add_executable(graph_draw_tests
    qt_app_fixture.cpp
    test_graphscene.cpp
    test_graphserializer.cpp
    test_dependencies.cpp
    test_repoanalyzer.cpp
)

target_link_libraries(graph_draw_tests PRIVATE
    graph_core
    graph_io
    graph_analyze
    Catch2::Catch2WithMain
    Qt${QT_VERSION_MAJOR}::Widgets
)

include(Catch)
catch_discover_tests(graph_draw_tests)
