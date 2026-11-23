cmake_minimum_required(VERSION 3.20)
project(redasm-mcp-plugin C)

if(NOT TARGET redasm::redasm)
    find_package(redasm REQUIRED)
endif()

add_subdirectory(mcp)
add_subdirectory(plugin)

redasm_install_plugins(mcp)
