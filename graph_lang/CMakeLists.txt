# Language-neutral analysis foundation.
#
# Qt Core ONLY. Never add graph_core or graph_io here — keeping scene types out
# of this library is the entire point of the module, and it is what lets the
# analyzer tests run without a QApplication.
add_library(graph_lang STATIC
    sourcetext.cpp        sourcetext.h
    repofileindex.cpp     repofileindex.h
    languageanalyzer.cpp  languageanalyzer.h
    ir/ir.cpp             ir/ir.h
)

target_include_directories(graph_lang PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ir
)

target_link_libraries(graph_lang PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
)
