#include "config.h"
#include "plugins.h"
#include <errno.h>
#include <poll.h>
#include <redasm/redasm.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READ_SIZE 4096
#define MAX_LINE_SIZE (16ull * 1024ull * 1024ull)
#define POLL_READY (POLLIN | POLLHUP | POLLERR | POLLNVAL)

typedef struct LineVect {
    char* data;
    size_t length;
    size_t capacity;
} LineVect;

typedef struct McpHost {
    const RDCommandPlugin* mcp;
    RDContext* context;
    int out; // the saved stdout
    bool analyzing;
} McpHost;

static bool write_all(int fd, const char* p, size_t n) {
    while(n) {
        ssize_t w = write(fd, p, n);

        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) return false;

        p += w;
        n -= (size_t)w;
    }

    return true;
}

static bool handle_line(McpHost* host, const char* line) {
    RDCommandValue args[2] = {
        {.kind = RD_CMDARG_STRING, .s = line},
        {.kind = RD_CMDARG_VOID},
    };

    RDCommandValue reply = host->mcp->execute(host->context, args);
    host->analyzing = true;

    // no reply: it was a notification
    if(reply.kind != RD_CMDARG_STRING || !reply.s || !reply.s[0]) return true;

    return write_all(host->out, reply.s, strlen(reply.s)) &&
           write_all(host->out, "\n", 1);
}

static bool line_reserve(LineVect* b, size_t n) {
    if(n <= b->capacity) return true;

    size_t capacity = b->capacity ? b->capacity * 2 : READ_SIZE;
    if(capacity < n) capacity = n;

    char* data = realloc(b->data, capacity);
    if(!data) return false;

    b->data = data;
    b->capacity = capacity;
    return true;
}

// splits a raw byte stream into lines: a read can hold half a line or several
static bool line_feed(McpHost* host, LineVect* lines, const char* data,
                      size_t n) {
    if(lines->length + n + 1 > MAX_LINE_SIZE) {
        fputs("[REDasm MCP] line too long, discarding\n", stderr);
        lines->length = 0;
    }

    if(!line_reserve(lines, lines->length + n + 1)) return false;
    memcpy(lines->data + lines->length, data, n);

    char* start = lines->data;
    char* scan = lines->data + lines->length; // earlier bytes hold no newline
    char* end = scan + n;
    char* nl;

    while((nl = memchr(scan, '\n', (size_t)(end - scan)))) {
        *nl = '\0';

        size_t len = (size_t)(nl - start);
        if(len && !handle_line(host, start)) return false;

        start = scan = nl + 1;
    }

    lines->length = (size_t)(end - start);
    memmove(lines->data, start, lines->length); // keep the unfinished line
    return true;
}

static int mcp_run(McpHost* host) {
    // a closed client must be a write error, not a silent death by signal
    signal(SIGPIPE, SIG_IGN);

    fputs("[REDasm MCP] ready\n", stderr);

    struct pollfd in = {.fd = STDIN_FILENO, .events = POLLIN};
    LineVect lines = {0};
    char buffer[READ_SIZE];
    int res = EXIT_SUCCESS;
    RDWorkerStatus status = {0};

    for(;;) {
        if(host->analyzing) host->analyzing = rd_step(host->context, &status);
        int r = poll(&in, 1, -1);

        if(r < 0) {
            if(errno == EINTR) continue;
            fputs("[REDasm MCP] poll() failed\n", stderr);
            res = EXIT_FAILURE;
            break;
        }

        if(!(in.revents & POLL_READY)) continue;

        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));

        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break; // the client closed stdin: time to exit

        if(!line_feed(host, &lines, buffer, (size_t)n)) {
            fputs("[REDasm MCP] cannot process input\n", stderr);
            res = EXIT_FAILURE;
            break;
        }
    }

    free(lines.data);
    fputs("[REDasm MCP] exiting\n", stderr);
    return res;
}

int main(int argc, char* argv[]) {
    if(argc != 3 || strcmp(argv[1], "--config") != 0) {
        fputs("usage: redasm_mcp --config <redasm_mcp.json>\n", stderr);
        return EXIT_FAILURE;
    }

    McpConfig config;
    if(!mcp_config_load(&config, argv[2])) return EXIT_FAILURE;

    // anything printing to stdout from now on lands on stderr
    int out = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);

    RDInitParams params = {.kb_paths = config.kb_dirs};
    rd_init(&params);

    size_t n = mcp_plugins_load(config.plugin_dirs);
    fprintf(stderr, "[REDasm MCP] loaded %zu plugin(s)\n", n);

    int status = EXIT_FAILURE;

    McpHost host = {
        .mcp = rd_command_find("mcp"),
        .out = out,
        .analyzing = true,
    };

    if(host.mcp)
        status = mcp_run(&host);
    else
        fputs("[REDasm MCP] 'mcp' command plugin not found\n", stderr);

    rd_deinit();
    mcp_config_destroy(&config);
    return status;
}
