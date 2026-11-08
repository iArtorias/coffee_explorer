# Windows 11 context menu. 'IExplorerCommand' in a COM library, served by the surrogate of the sparse package 'ui\CoffeeExplorer\Package\AppxManifest.xml'.
# Built with the core so it lands next to 'pecore.dll'.
add_library(CoffeeExplorer.Shell SHARED ContextMenu.cpp ContextMenu.def)
target_compile_definitions(CoffeeExplorer.Shell PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(CoffeeExplorer.Shell PRIVATE shlwapi ole32 oleaut32 shell32 uuid)
set_target_properties(CoffeeExplorer.Shell PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    target_compile_options(CoffeeExplorer.Shell PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()