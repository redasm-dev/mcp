cmake_minimum_required(VERSION 3.20)

project(mcp C)

if(NOT TARGET redasm::redasm)
    find_package(redasm REQUIRED)
endif()

include(cmake/Dependencies.cmake)
setup_dependencies()

add_library(${PROJECT_NAME} SHARED module.c)

target_sources(${PROJECT_NAME}
    PRIVATE 
        spec/def.c
        spec/entries.c
        mcp.c
        session.c
        table.c
        tools.c
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} 
    PRIVATE 
        redasm::redasm
        yyjson
)
