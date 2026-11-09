cmake_minimum_required(VERSION 3.20)
project(redasm-mcp C)

if(WIN32)
    set(REDASM_INSTALL_PLUGINS "plugins/mcp")
else()
    include(GNUInstallDirs)
    set(REDASM_INSTALL_PLUGINS "${CMAKE_INSTALL_LIBDIR}/redasm/plugins/mcp")
endif()

add_subdirectory(mcp)
add_subdirectory(plugin)

get_property(subdirs DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)

foreach(subdir ${subdirs})
    get_property(targets DIRECTORY ${subdir} PROPERTY BUILDSYSTEM_TARGETS)

    foreach(target ${targets})
        get_target_property(target_type ${target} TYPE)

        if(target_type STREQUAL "SHARED_LIBRARY")
            if(NOT WIN32)
                set_target_properties(${target} PROPERTIES
                                      INSTALL_RPATH "$ORIGIN/../../../"
                )
            endif()

            install(TARGETS ${target}
                    LIBRARY DESTINATION "${REDASM_INSTALL_PLUGINS}"
                    RUNTIME DESTINATION "${REDASM_INSTALL_PLUGINS}"
            )
        endif()
    endforeach()
endforeach()
