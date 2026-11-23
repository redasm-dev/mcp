cmake_minimum_required(VERSION 3.20)
project(redasm-mcp-plugin C)

if(WIN32)
    set(REDASM_INSTALL_PLUGINS "plugins/mcp")
else()
    include(GNUInstallDirs)
    set(REDASM_INSTALL_PLUGINS "${CMAKE_INSTALL_LIBDIR}/redasm/plugins/mcp")
endif()

add_subdirectory(mcp)
add_subdirectory(plugin)

redasm_install_plugins(mcp)
