add_library(graph_lang_rust STATIC
    scanRustFile.cpp
)

target_include_directories(graph_lang_rust PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(graph_lang_rust PUBLIC
    graph_core
)
