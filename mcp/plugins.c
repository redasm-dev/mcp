#include "plugins.h"

#include <redasm/redasm.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define PLUGIN_EXT ".dll"
#elif defined(__APPLE__)
#define PLUGIN_EXT ".dylib"
#else
#define PLUGIN_EXT ".so"
#endif

#define MAX_DEPTH 16

static size_t _plugins_walk(const char* dir, int depth);

static char* _plugins_join(const char* a, const char* b) {
    size_t na = strlen(a), nb = strlen(b);
    char* path = malloc(na + nb + 2);
    if(!path) return NULL;

    memcpy(path, a, na);
    path[na] = '/';
    memcpy(path + na + 1, b, nb + 1);
    return path;
}

static bool _plugins_has_ext(const char* name) {
    size_t n = strlen(name), next = strlen(PLUGIN_EXT);
    return n > next && !strcmp(name + n - next, PLUGIN_EXT);
}

static size_t _plugins_load_file(const char* path) {
    if(rd_module_load(path)) return 1;
    fprintf(stderr, "[REDasm MCP] cannot load plugin '%s'\n", path);
    return 0;
}

#if defined(_WIN32)

// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format on

static wchar_t* _plugins_to_wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if(n <= 0) return NULL;

    wchar_t* w = malloc((size_t)n * sizeof(*w));
    if(w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char* _plugins_to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if(n <= 0) return NULL;

    char* s = malloc((size_t)n);
    if(s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static size_t _plugins_walk(const char* dir, int depth) {
    if(depth > MAX_DEPTH) return 0;

    char* pattern = _plugins_join(dir, "*");
    wchar_t* wpattern = pattern ? _plugins_to_wide(pattern) : NULL;
    free(pattern);
    if(!wpattern) return 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    free(wpattern);

    if(h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[REDasm MCP] cannot read directory '%s'\n", dir);
        return 0;
    }

    size_t count = 0;

    do {
        if(!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        char* name = _plugins_to_utf8(fd.cFileName);
        char* path = name ? _plugins_join(dir, name) : NULL;

        if(path) {
            if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                count += _plugins_walk(path, depth + 1);
            else if(_plugins_has_ext(name))
                count += _plugins_load_file(path);
        }

        free(path);
        free(name);
    } while(FindNextFileW(h, &fd));

    FindClose(h);
    return count;
}

#else

#include <dirent.h>
#include <sys/stat.h>

static size_t _plugins_walk(const char* dir, int depth) {
    if(depth > MAX_DEPTH) return 0;

    DIR* d = opendir(dir);

    if(!d) {
        fprintf(stderr, "[REDasm MCP] cannot read directory '%s'\n", dir);
        return 0;
    }

    size_t count = 0;
    const struct dirent* e;

    while((e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char* path = _plugins_join(dir, e->d_name);
        if(!path) continue;

        struct stat st;

        // symlinked files are fine, symlinked directories could loop
        if(lstat(path, &st) == 0) {
            if(S_ISDIR(st.st_mode)) {
                count += _plugins_walk(path, depth + 1);
            }
            else if(_plugins_has_ext(e->d_name) && stat(path, &st) == 0 &&
                    S_ISREG(st.st_mode)) {
                count += _plugins_load_file(path);
            }
        }

        free(path);
    }

    closedir(d);
    return count;
}

#endif

size_t mcp_plugins_load(const char** dirs) {
    size_t count = 0;

    for(const char** p = dirs; *p; p++)
        count += _plugins_walk(*p, 0);

    return count;
}
