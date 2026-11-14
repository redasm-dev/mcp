cmake_minimum_required(VERSION 3.20)

project(mcp C)

if(NOT TARGET redasm::redasm)
    find_package(redasm REQUIRED)
endif()

include(cmake/Dependencies.cmake)
setup_dependencies()

add_subdirectory(gen_tools)
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")

add_custom_command(
    OUTPUT "${GEN_DIR}/tools_gen.c" "${GEN_DIR}/tools_gen.h"
    COMMAND gen_tools "${CMAKE_CURRENT_SOURCE_DIR}/gen_tools/spec.json" "${GEN_DIR}"
    DEPENDS gen_tools gen_tools/spec.json
    COMMENT "Generating MCP tool bindings"
    VERBATIM
)

add_library(${PROJECT_NAME} SHARED module.c)

target_sources(${PROJECT_NAME}
    PRIVATE 
        executors.c
        mcp.c
        session.c
        table.c
        tools.c

    PRIVATE
        "${GEN_DIR}/tools_gen.c"
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GEN_DIR}"
)

target_link_libraries(${PROJECT_NAME} 
    PRIVATE 
        redasm::redasm
        yyjson
)
