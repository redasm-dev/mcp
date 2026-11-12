#include "entries.h"
#include "mcp.h"
#include "session.h"
#include "table.h"
#include "tools.h"
#include <ctype.h>
#include <inttypes.h>

MCP_ENUM(external_kind, RDExternalKind, EXTERNAL_KINDS);
MCP_ENUM(symbol_kind, u32, SYMBOL_KINDS);
MCP_ENUM(xref_kind, u32, XREF_KINDS);

static bool _mcp_spec_is_allowed(const char* resolved) {
    const RDDatum* dirs =
        rd_datum_get_array(rd_settings_global(), "mcp.allowed_dirs");

    const RDDatum* d;
    rd_datum_each(d, dirs) {
        const char* dir = rd_datum_to_str(d);
        if(dir && rd_path_isinside(resolved, dir)) return true;
    }

    return false;
}

static const char* _mcp_spec_format_address(const RDContext* ctx, RDAddress a) {
    int width = (int)rd_get_ptr_size(ctx) * 2;
    return rd_format("0x%0*" PRIX64, width > 0 ? width : 8, (u64)a);
}

// the 'type' column: single kind bits have a name in XREF_KINDS
static const char* _mcp_spec_xref_type(RDXRefType type) {
    u32 bit = XR_BIT(type);
    return MCP_GET_ENUM_NAME(xref_kind, bit);
}

// the name of the function containing 'address', NULL outside functions
static const char* _mcp_spec_function_of(RDContext* ctx, RDAddress address,
                                         RDAddress* entry) {
    if(!rd_function_find(ctx, address, entry)) return NULL;

    const char* name = rd_get_name(ctx, *entry);
    return (name && *name) ? name : NULL;
}

// accepts "0x401000", a name such as "main" or "sub_401000", or plain hex
static bool _mcp_spec_parse_address(RDContext* ctx, const char* s,
                                    RDAddress* out) {
    bool hexprefix = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');

    // a name first: "add" or "face" are valid hex too
    if(!hexprefix && rd_get_address(ctx, s, out)) return true;

    const char* digits = hexprefix ? s + 2 : s;
    if(!(*digits) || strlen(digits) > 16) return false;

    for(const char* p = digits; *p; p++) {
        if(!isxdigit((unsigned char)*p)) return false;
    }

    *out = (RDAddress)strtoull(digits, NULL, 16);
    return true;
}

// the facts about an address: segment, function, external
static void _mcp_describe_address(RDContext* ctx, RDAddress address,
                                  RDScratchBuffer* out) {
    const RDSegment* seg = rd_segment_find(ctx, address);

    if(!seg) {
        rd_scratch_puts(out, "not inside any segment");
        return;
    }

    rd_scratch_puts(out, rd_is_function(ctx, address) ? "function" : "address");
    rd_scratch_putf(out, " in segment %s", rd_segment_get_name(seg));

    RDExternal ext;
    if(!rd_get_external(ctx, address, &ext)) return;

    if(ext.kind == RD_EXT_EXPORTED)
        rd_scratch_puts(out, ", exported");
    else
        rd_scratch_putf(out, ", imported from %s",
                        ext.module ? ext.module : "?");
}

static void _mcp_spec_table_location(McpTable* t, const char* fname,
                                     RDAddress entry, RDAddress address) {
    if(!fname)
        mcp_table_str(t, NULL);
    else if(entry == address)
        mcp_table_str(t, fname);
    else
        mcp_table_fmt(t, "%s+0x%" PRIX64, fname, (u64)(address - entry));
}

static yyjson_mut_val* _mcp_spec_get_calls(RDContext* ctx, yyjson_val* args,
                                           yyjson_mut_doc* doc, bool callees) {
    const char* s = yyjson_get_str(yyjson_obj_get(args, "address"));
    RDAddress address, entry;

    if(!_mcp_spec_parse_address(ctx, s, &address)) {
        return mcp_result_error(doc, "Not an address or a known name: use hex "
                                     "like 0x401000 or a symbol name.");
    }

    if(!rd_function_find(ctx, address, &entry))
        return mcp_result_error(doc, "This address is not inside a function.");

    McpPage page;
    const char* err = mcp_page_init(&page, args);
    if(err) return mcp_result_error(doc, err);

    static const char* const CALLEES[] = {"from", "call", "to", "name", NULL};
    static const char* const CALLERS[] = {"from", "call", "function", NULL};

    McpTable t = mcp_table_begin(doc, callees ? CALLEES : CALLERS);
    mcp_table_set_untrusted(&t);

    RDCallSlice calls =
        callees ? rd_get_callees(ctx, address) : rd_get_callers(ctx, address);

    const RDCall* call;
    rd_slice_each(call, calls) {
        const char* kind = call->type == RD_CR_CALL ? "direct" : "indirect";

        if(callees) {
            const char* name = rd_get_name(ctx, call->target);
            if(name && !*name) name = NULL;
            if(!mcp_page_take(&page, name)) continue;

            mcp_table_row(&t);
            mcp_table_address(&t, ctx, call->address);
            mcp_table_str(&t, kind);
            mcp_table_address(&t, ctx, call->target);
            mcp_table_str(&t, name);
        }
        else {
            RDAddress caller;
            const char* fname =
                _mcp_spec_function_of(ctx, call->address, &caller);
            if(!mcp_page_take(&page, fname)) continue;

            mcp_table_row(&t);
            mcp_table_address(&t, ctx, call->address);
            mcp_table_str(&t, kind);
            _mcp_spec_table_location(&t, fname, caller, call->address);
        }
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_spec_status(RDContext* ctx, yyjson_val* args,
                                yyjson_mut_doc* doc) {
    RD_UNUSED(args);
    if(!ctx) return mcp_result_text(doc, "No file open.");

    const RDLoaderPlugin* loader = rd_get_loader_plugin(ctx);
    const RDProcessorPlugin* processor = rd_get_processor_plugin(ctx);

    const char* text =
        rd_format("File: %s\nLoader: %s\nProcessor: %s", rd_get_file_name(ctx),
                  loader ? loader->id : "?", processor ? processor->id : "?");

    return mcp_result_text(doc, text);
}

yyjson_mut_val* mcp_spec_open_file(RDContext* ctx, yyjson_val* args,
                                   yyjson_mut_doc* doc) {
    RD_UNUSED(ctx);

    if(!mcp_session_is_standalone())
        return mcp_result_error(
            doc, "Opening files is only available in redasm_mcp.");

    if(!mcp_session_settings_valid())
        return mcp_result_error(
            doc, "Opening files is disabled: the [mcp] settings are invalid.");

    if(rd_datum_is_empty(
           rd_datum_get_array(rd_settings_global(), "mcp.allowed_dirs")))
        return mcp_result_error(
            doc, "Opening files is disabled: no allowed_dirs set.");

    // validated: present and a string
    const char* path = yyjson_get_str(yyjson_obj_get(args, "path"));
    if(!(*path)) return mcp_result_error(doc, "'path' cannot be empty.");

    // resolve first, then check and open the same resolved path
    const char* r = rd_path_resolve(path);
    if(!r) return mcp_result_error(doc, "File not found.");

    char* resolved = rd_strdup(r); // rd_path_resolve's buffer is shared
    yyjson_mut_val* result;

    if(!rd_path_isfile(resolved)) {
        result = mcp_result_error(doc, "Not a regular file.");
    }
    else if(!_mcp_spec_is_allowed(resolved)) {
        result =
            mcp_result_error(doc, "Path is outside the allowed directories.");
    }
    else if(!mcp_session_open(resolved)) {
        result = mcp_result_error(
            doc, "Cannot open this file, no file is open now.");
    }
    else {
        const char* text = rd_format(
            "Opened %s, analysis complete. Addresses and names from any "
            "previous file no longer apply.",
            rd_path_filename(resolved));
        result = mcp_result_text(doc, text);
    }

    rd_free(resolved);
    return result;
}

yyjson_mut_val* mcp_spec_list_segments(RDContext* ctx, yyjson_val* args,
                                       yyjson_mut_doc* doc) {
    RD_UNUSED(args);

    static const char* const COLUMNS[] = {
        "start", "end", "size", "perm", "name", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDSegmentSlice segments = rd_get_all_segments(ctx);

    const RDSegment** it;
    rd_slice_each(it, segments) {
        const RDSegment* seg = *it;
        RDSegmentPerm p = rd_segment_get_perm(seg);

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, rd_segment_get_start(seg));
        mcp_table_address(&t, ctx, rd_segment_get_end(seg));
        mcp_table_uint(&t, rd_segment_get_size(seg));
        mcp_table_fmt(&t, "%c%c%c", (p & RD_SP_R) ? 'r' : '-',
                      (p & RD_SP_W) ? 'w' : '-', (p & RD_SP_X) ? 'x' : '-');
        mcp_table_str(&t, rd_segment_get_name(seg));
    }

    return mcp_table_result(&t, NULL);
}

yyjson_mut_val* mcp_spec_list_functions(RDContext* ctx, yyjson_val* args,
                                        yyjson_mut_doc* doc) {
    McpPage page;
    const char* err = mcp_page_init(&page, args);
    if(err) return mcp_result_error(doc, err);

    static const char* const COLUMNS[] = {"address", "name", NULL};

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDAddressSlice functions = rd_get_all_functions(ctx);

    const RDAddress* it;
    rd_slice_each(it, functions) {
        // rd_get_name's buffer is reused: consume it before the next call
        const char* name = rd_get_name(ctx, *it);
        if(!mcp_page_take(&page, name)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, *it);
        mcp_table_str(&t, name);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_spec_list_mappings(RDContext* ctx, yyjson_val* args,
                                       yyjson_mut_doc* doc) {
    RD_UNUSED(args);

    static const char* const COLUMNS[] = {
        "offset", "start", "end", "size", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    RDInputMappingSlice mappings = rd_get_all_mappings(ctx);

    const RDInputMapping** it;
    rd_slice_each(it, mappings) {
        mcp_table_row(&t);
        mcp_table_fmt(&t, "0x%" PRIX64, (u64)rd_inputmapping_get_offset(*it));
        mcp_table_address(&t, ctx, rd_inputmapping_get_start(*it));
        mcp_table_address(&t, ctx, rd_inputmapping_get_end(*it));
        mcp_table_uint(&t, rd_inputmapping_get_size(*it));
    }

    return mcp_table_result(&t, NULL);
}

yyjson_mut_val* mcp_spec_list_externals(RDContext* ctx, yyjson_val* args,
                                        yyjson_mut_doc* doc) {
    McpPage page;
    const char* err = mcp_page_init(&page, args);
    if(err) return mcp_result_error(doc, err);

    RDExternalKind kind = MCP_GET_ENUM_VAL(args, "kind", external_kind);

    static const char* const COLUMNS[] = {
        "address", "kind", "module", "name", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDExternalSlice externals = rd_get_all_externals(ctx, kind);

    const RDExternal* ext;
    rd_slice_each(ext, externals) {
        const char* name = rd_get_name(ctx, ext->address);
        if(!mcp_page_take(&page, name)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, ext->address);
        mcp_table_str(&t, ext->kind == RD_EXT_EXPORTED ? "export" : "import");
        mcp_table_str(&t, ext->module);

        // imports by ordinal often have no name
        if(name && *name)
            mcp_table_str(&t, name);
        else if(ext->ordinal.has_value)
            mcp_table_fmt(&t, "#%u", ext->ordinal.value);
        else
            mcp_table_str(&t, NULL);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_spec_list_symbols(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc) {
    McpPage page;
    const char* err = mcp_page_init(&page, args);
    if(err) return mcp_result_error(doc, err);

    static const char* const COLUMNS[] = {
        "address", "kind", "type", "name", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    u32 mask = MCP_GET_ENUM_VAL(args, "kind", symbol_kind);
    RDSymbolSlice symbols = rd_get_all_symbols(ctx);

    const RDSymbol* sym;
    rd_slice_each(sym, symbols) {
        if(!(mask & SYM_BIT(sym->kind))) continue;

        // rd_get_name's buffer is reused: consume it before the next call
        const char* name = sym->kind == RD_SYMBOL_SEGMENT
                               ? rd_segment_get_name(sym->segment)
                               : rd_get_name(ctx, sym->address);

        if(!mcp_page_take(&page, name)) continue;

        // copied now: rd_type_to_str below may reuse the same buffers
        yyjson_mut_val* nameval =
            name ? mcp_str(doc, name, strlen(name)) : NULL;

        u32 bit = SYM_BIT(sym->kind);

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, sym->address);
        mcp_table_str(&t, MCP_GET_ENUM_NAME(symbol_kind, bit));
        mcp_table_str(&t, sym->kind == RD_SYMBOL_TYPE
                              ? rd_type_to_str(&sym->type, ctx)
                              : NULL);
        mcp_table_val(&t, nameval);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_spec_get_name(RDContext* ctx, yyjson_val* args,
                                  yyjson_mut_doc* doc) {
    const char* s = yyjson_get_str(yyjson_obj_get(args, "address"));
    RDAddress address;

    if(!_mcp_spec_parse_address(ctx, s, &address)) {
        return mcp_result_error(doc, "Not an address or a known name: use hex "
                                     "like 0x401000 or a symbol name.");
    }

    RDScratchBuffer* out = rd_scratch_create();
    rd_scratch_putf(out, "%s: ", _mcp_spec_format_address(ctx, address));

    const char* name = rd_get_name(ctx, address);
    rd_scratch_puts(out, (name && *name) ? name : "no name");

    // auto labels like loc_401130 say little: the enclosing function is
    // usually what's being asked
    RDAddress entry;

    if(rd_function_find(ctx, address, &entry) && entry != address) {
        const char* fname = rd_get_name(ctx, entry);
        rd_scratch_putf(out, ", inside %s+0x%" PRIX64,
                        (fname && *fname) ? fname : "?",
                        (u64)(address - entry));
    }

    rd_scratch_puts(out, " (");
    _mcp_describe_address(ctx, address, out);
    rd_scratch_puts(out, ")\nNote: names come from the analyzed binary and may "
                         "be misleading.");

    yyjson_mut_val* r =
        mcp_result_text_n(doc, rd_scratch_data(out), rd_scratch_length(out));
    rd_scratch_destroy(out);
    return r;
}

yyjson_mut_val* mcp_spec_get_address(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc) {
    const char* name = yyjson_get_str(yyjson_obj_get(args, "name"));
    RDAddress address;

    if(!rd_get_address(ctx, name, &address)) {
        return mcp_result_error(
            doc, rd_format("No symbol named '%.100s'. Use list_functions or "
                           "list_symbols with a filter to search.",
                           name));
    }

    RDScratchBuffer* out = rd_scratch_create();
    rd_scratch_putf(out, "%.200s = %s (", name,
                    _mcp_spec_format_address(ctx, address));
    _mcp_describe_address(ctx, address, out);
    rd_scratch_putchar(out, ')');

    yyjson_mut_val* r =
        mcp_result_text_n(doc, rd_scratch_data(out), rd_scratch_length(out));
    rd_scratch_destroy(out);
    return r;
}

yyjson_mut_val* mcp_spec_get_xrefs_to(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc) {
    const char* s = yyjson_get_str(yyjson_obj_get(args, "address"));
    RDAddress address;

    if(!_mcp_spec_parse_address(ctx, s, &address)) {
        return mcp_result_error(doc, "Not an address or a known name: use hex "
                                     "like 0x401000 or a symbol name.");
    }

    McpPage page;
    const char* err = mcp_page_init(&page, args);
    if(err) return mcp_result_error(doc, err);

    u32 mask = MCP_GET_ENUM_VAL(args, "kind", xref_kind);

    static const char* const COLUMNS[] = {"from", "type", "function", NULL};

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    // the slice lives in the context until the next rd_get_xrefs_to():
    // rd_function_find and rd_get_name below don't touch it
    RDXRefSlice refs = rd_get_xrefs_to(ctx, address, RD_XR_NONE);

    const RDXRef* xr;
    rd_slice_each(xr, refs) {
        if(!(mask & XR_BIT(xr->type))) continue;

        RDAddress entry;
        const char* fname = _mcp_spec_function_of(ctx, xr->address, &entry);
        if(!mcp_page_take(&page, fname)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, xr->address);
        mcp_table_str(&t, _mcp_spec_xref_type(xr->type));

        _mcp_spec_table_location(&t, fname, entry, xr->address);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_spec_get_xrefs_from(RDContext* ctx, yyjson_val* args,
                                        yyjson_mut_doc* doc) {
    const char* s = yyjson_get_str(yyjson_obj_get(args, "address"));
    RDAddress address;

    if(!_mcp_spec_parse_address(ctx, s, &address)) {
        return mcp_result_error(doc, "Not an address or a known name: use hex "
                                     "like 0x401000 or a symbol name.");
    }

    McpPage page;
    const char* err = mcp_page_init(&page, args);
    if(err) return mcp_result_error(doc, err);

    u32 mask = MCP_GET_ENUM_VAL(args, "kind", xref_kind);

    static const char* const COLUMNS[] = {"to", "type", "name", NULL};

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    // same lifetime as in get_xrefs_to, with rd_get_xrefs_from()
    RDXRefSlice refs = rd_get_xrefs_from(ctx, address, RD_XR_NONE);

    const RDXRef* xr;
    rd_slice_each(xr, refs) {
        if(!(mask & XR_BIT(xr->type))) continue;

        const char* name = rd_get_name(ctx, xr->address);
        if(name && !*name) name = NULL;
        if(!mcp_page_take(&page, name)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, xr->address);
        mcp_table_str(&t, _mcp_spec_xref_type(xr->type));
        mcp_table_str(&t, name);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_spec_get_callees(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc) {
    return _mcp_spec_get_calls(ctx, args, doc, true);
}

yyjson_mut_val* mcp_spec_get_callers(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc) {
    return _mcp_spec_get_calls(ctx, args, doc, false);
}
