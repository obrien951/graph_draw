add_library(graph_analyze STATIC
    repoanalyzer.cpp   repoanalyzer.h
    graphbuilder.cpp   graphbuilder.h
)

# PUBLIC so that consumers of graph_analyze also see this header
target_include_directories(graph_analyze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Builds a GraphScene (graph_core) from an ir::Repo produced by a language
# plugin. No graph_io: serialisation is the caller's business.
target_link_libraries(graph_analyze PUBLIC
    graph_core
    graph_lang
    graph_lang_cpp
    Qt${QT_VERSION_MAJOR}::Widgets
)
