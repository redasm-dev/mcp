redasm_add_dependency(
    NAME cJSON
    VERSION "1.7.19"
    GITHUB_REPOSITORY "DaveGamble/cJSON"
    DOWNLOAD_ONLY YES

    OPTIONS 
        "BUILD_SHARED_LIBS=OFF"
        "ENABLE_CJSON_TEST=OFF"
)

if(cJSON_ADDED)
    add_library(cJSON STATIC "${cJSON_SOURCE_DIR}/cJSON.c")
    target_include_directories(cJSON PUBLIC "${cJSON_SOURCE_DIR}")
    set_target_properties(cJSON PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
