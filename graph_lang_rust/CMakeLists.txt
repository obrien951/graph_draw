add_library(graph_lang_rust STATIC
    rust_analyzer.cpp
    stubs.cpp
    toml_document.cpp
    toml_document.h
    rustlex.h
    rust_file_items.h
    rust_analyzer.h
)

target_include_directories(graph_lang_rust PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(graph_lang_rust PUBLIC
    graph_lang
)
