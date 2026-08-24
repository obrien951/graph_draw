add_library(graph_lang STATIC
    sourcetext.cpp
)

target_include_directories(graph_lang PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(graph_lang PUBLIC Qt6::Core)
