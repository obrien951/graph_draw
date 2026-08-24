add_library(graph_merge STATIC
    graphmerge.cpp   graphmerge.h
    graphmerger.cpp  graphmerger.h
)

target_include_directories(graph_merge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(graph_merge PUBLIC graph_core)
