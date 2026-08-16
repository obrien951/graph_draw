add_library(graph_analyze STATIC
    repoanalyzer.cpp   repoanalyzer.h
)

# PUBLIC so that consumers of graph_analyze also see this header
target_include_directories(graph_analyze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Builds a GraphScene (graph_core) and serialises it (graph_io).
target_link_libraries(graph_analyze PUBLIC
    graph_core
    graph_io
    Qt${QT_VERSION_MAJOR}::Widgets
)
