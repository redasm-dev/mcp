cmake_minimum_required(VERSION 3.20)

project(redasm_mcp C)

if(NOT TARGET redasm::redasm)
    find_package(redasm REQUIRED)
endif()


add_executable(${PROJECT_NAME} main.c)

target_sources(${PROJECT_NAME}
    PRIVATE
        config.c
        plugins.c
)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        EXPORT_COMPILE_COMMANDS ON
        C_STANDARD 17
)

target_link_libraries(${PROJECT_NAME} 
    PRIVATE 
        redasm::redasm
)
