add_library(graph_lang_cpp STATIC
    CppAnalyzer.cpp
)

target_include_directories(graph_lang_cpp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(graph_lang_cpp PUBLIC
    graph_core
    graph_io
)
