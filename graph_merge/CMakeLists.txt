add_library(graph_merge STATIC
    graphmerger.cpp   graphmerger.h
)

# PUBLIC so that consumers of graph_merge also see this header
target_include_directories(graph_merge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# graph_merge folds one GraphScene into another. It intentionally depends on
# graph_core only, so it can be tested with two in-memory scenes.
target_link_libraries(graph_merge PUBLIC
    graph_core
    Qt${QT_VERSION_MAJOR}::Widgets
)
