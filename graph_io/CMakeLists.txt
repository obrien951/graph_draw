add_library(graph_io STATIC
    graphserializer.cpp graphserializer.h
)

target_include_directories(graph_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# graph_io depends on graph_core to know about GraphScene, GraphNode, GraphEdge
target_link_libraries(graph_io PUBLIC graph_core)
