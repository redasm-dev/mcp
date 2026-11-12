#include "session.h"
#include <string.h>

static RDContext* mcp_own_ctx = NULL;
static char* mcp_pending_file = NULL;
static bool mcp_standalone = false;
static bool mcp_settings_valid = true;
static bool mcp_debug = false;

static const RDDatumSchema MCP_SETTINGS_SCHEMA[] = {
    {
        .key = "allowed_dirs",
        .kind = RD_DATUM_ARRAY,
        .array_type = &RD_DATUM_ITEM_STRING,
        .optional = true,
    },
    {
        .key = "working_dir",
        .kind = RD_DATUM_STRING,
        .optional = true,
    },
    {
        .key = "debug",
        .kind = RD_DATUM_BOOL,
        .optional = true,
    },
    {0},
};

static const char* _mcp_session_working_dir(void) {
    return rd_datum_get_str(rd_settings_global(), "mcp.working_dir");
}

static void _mcp_session_destroy(void) {
    rd_destroy(mcp_own_ctx);
    mcp_own_ctx = NULL;
}

bool mcp_session_open(const char* filepath) {
    if(!mcp_settings_valid) return false;

    _mcp_session_destroy();
    if(!filepath || !(*filepath)) return false;

    RDTestResultSlice tests = rd_test(filepath);

    if(rd_slice_is_empty(tests)) {
        RD_LOG_FAIL("no loader for '%s'", filepath);
        return false;
    }

    RDAcceptParams params = {
        .mode = RD_AM_NEW,
        .working_dir = _mcp_session_working_dir(),
    };

    RDAcceptResult res = rd_accept(rd_slice_at(tests, 0), &params);

    if(res.status != RD_ACCEPT_OK) {
        RD_LOG_FAIL("cannot open '%s'", filepath);
        return false;
    }

    mcp_own_ctx = res.context;
    rd_run(mcp_own_ctx);
    return true;
}

RDCommandValue mcp_session_open_file(RDContext* ctx,
                                     const RDCommandValue* args) {
    RD_UNUSED(ctx);

    // only the standalone host calls this, the GUI passes its own context.
    // Set first: with bad settings the open_file tool must report those, not
    // "only available in redasm_mcp"
    mcp_standalone = true;

    if(!mcp_settings_valid)
        return (RDCommandValue){.kind = RD_CMDARG_BOOL, .b = false};

    const char* wd = _mcp_session_working_dir();

    if(wd && !rd_path_isdir(wd)) {
        RD_LOG_FAIL("working_dir '%s' is not a directory", wd);
        return (RDCommandValue){.kind = RD_CMDARG_BOOL, .b = false};
    }

    _mcp_session_destroy();

    rd_free(mcp_pending_file);
    const char* path = args[0].s;
    mcp_pending_file = (path && *path) ? rd_strdup(args[0].s) : NULL;

    return (RDCommandValue){.kind = RD_CMDARG_BOOL, .b = true};
}

RDContext* mcp_session_context(RDContext* host_ctx) {
    if(host_ctx) return host_ctx;
    if(!mcp_standalone) return NULL;

    // opened on the first tool call, so the handshake never waits for analysis
    if(mcp_pending_file) {
        char* path = mcp_pending_file;
        mcp_pending_file = NULL;
        mcp_session_open(path);
        rd_free(path);
    }

    return mcp_own_ctx;
}

bool mcp_session_debug(void) { return mcp_debug; }
bool mcp_session_settings_valid(void) { return mcp_settings_valid; }
bool mcp_session_is_standalone(void) { return mcp_standalone; }

void mcp_session_close(void) {
    _mcp_session_destroy();
    rd_free(mcp_pending_file);
    mcp_pending_file = NULL;
}

void mcp_session_init(void) {
    const RDDatum* mcp = rd_datum_get_table(rd_settings_global(), "mcp");

    // no [mcp] table is fine: every key is optional
    mcp_settings_valid = !mcp || rd_datum_validate(mcp, MCP_SETTINGS_SCHEMA);

    if(mcp_settings_valid) {
        mcp_debug = false;
        rd_datum_get_bool(rd_settings_global(), "mcp.debug", &mcp_debug);
    }
    else {
        RD_LOG_FAIL("%s", "invalid [mcp] settings, opening files is disabled");
    }
}
