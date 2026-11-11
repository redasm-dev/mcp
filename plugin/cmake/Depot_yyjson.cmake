redasm_add_dependency(
    NAME yyjson
    GIT_TAG "0.13.0"
    GITHUB_REPOSITORY "ibireme/yyjson"
    DOWNLOAD_ONLY YES
)

if(yyjson_ADDED)
    add_library(yyjson STATIC "${yyjson_SOURCE_DIR}/src/yyjson.c")
    target_include_directories(yyjson PUBLIC "${yyjson_SOURCE_DIR}/src")
    set_target_properties(yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
