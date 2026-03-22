add_library(graph_core STATIC
    graphnode.cpp   graphnode.h
    graphedge.cpp   graphedge.h
    graphscene.cpp  graphscene.h
    graphview.cpp   graphview.h
)

# PUBLIC so that consumers of graph_core also see these headers
target_include_directories(graph_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(graph_core PUBLIC Qt${QT_VERSION_MAJOR}::Widgets)
