# The C++ language plugin.
#
# Links graph_lang ONLY (which is Qt Core only). Never graph_core or graph_io:
# an analyzer describes what it found in the IR and never builds scene items.
add_library(graph_lang_cpp STATIC
    cppanalyzer.cpp   cppanalyzer.h
)

target_include_directories(graph_lang_cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(graph_lang_cpp PUBLIC
    graph_lang
)
