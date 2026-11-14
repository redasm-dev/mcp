// gen_tools: generates the MCP tool bindings from spec/tools.json
//
//   gen_tools <tools.json> <output dir>
//
// Writes into the output dir:
//   tools_gen.h  enums, argument accessors, executor prototypes
//   tools_gen.c  dispatch table, the 'tools/list' JSON, accessors, enum names
//
// The spec is MCP's own tool format plus a few extensions, which are removed
// from the 'tools/list' reply:
//   tool "x-context": true       the tool needs an open file
//   tool "x-paging": true        adds the arguments in "paging"
//   argument "x-enum": "<name>"  a value of an enum, expanded to "enum"
//   argument "x-only": [...]     a subset of that enum
//
// Enums:
//   "values": ["a", "b"]                 plugin-only, the C enum is generated
//   "type": "T", "values": {"a": "C"}    mapped to existing C constants of T
//
// Every argument gets an accessor, 'mcp_arg_<tool>_<arg>(args)', which returns
// its value or its default; paging arguments share 'mcp_paging_<arg>(args)'.
// Executors are 'mcp_exec_<tool>' and are written by hand in executors.c.
// This is a build tool: on any error it reports and exits.

#include <yyjson.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* spec_path;

static void fail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "gen_tools: %s: ", spec_path);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    exit(EXIT_FAILURE);
}

static void* xrealloc(void* p, size_t n) {
    void* np = realloc(p, n ? n : 1);

    if(!np) {
        fputs("gen_tools: out of memory\n", stderr);
        exit(EXIT_FAILURE);
    }

    return np;
}

// ---- text output ------------------------------------------------------------

typedef struct Buf {
    char* data;
    size_t len;
    size_t cap;
} Buf;

static void buf_putf(Buf* b, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if(n < 0) fail("formatting failed");

    size_t need = b->len + (size_t)n + 1;

    if(need > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while(cap < need)
            cap *= 2;

        b->data = xrealloc(b->data, cap);
        b->cap = cap;
    }

    va_start(args, fmt);
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, args);
    va_end(args);
    b->len += (size_t)n;
}

// every string built by str_f, released by pool_free()
static char** pool;
static size_t npool;

static void pool_free(void) {
    for(size_t i = 0; i < npool; i++)
        free(pool[i]);

    free((void*)pool);
    pool = NULL;
    npool = 0;
}

// a formatted string, owned by the pool
static char* str_f(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if(n < 0) fail("formatting failed");

    char* s = xrealloc(NULL, (size_t)n + 1);
    va_start(args, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, args);
    va_end(args);

    pool = (char**)xrealloc((void*)pool, (npool + 1) * sizeof(*pool));
    pool[npool++] = s;
    return s;
}

static char* str_upper(const char* s) {
    char* u = str_f("%s", s);

    for(char* p = u; *p; p++) {
        if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }

    return u;
}

// comment_placement -> McpCommentPlacement
static char* str_camel(const char* s) {
    char* c = str_f("Mcp%s", s);
    char* out = c + 3;
    bool up = true;

    for(const char* p = s; *p; p++) {
        if(*p == '_') {
            up = true;
            continue;
        }

        *out++ = (up && *p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
        up = false;
    }

    *out = 0;
    return c;
}

static void buf_free(Buf* b) {
    free(b->data);
    *b = (Buf){0};
}

// "..." with C escapes
static void buf_put_cstr(Buf* b, const char* s) {
    buf_putf(b, "\"");

    for(const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch(*p) {
            case '\\': buf_putf(b, "\\\\"); break;
            case '"': buf_putf(b, "\\\""); break;
            case '\n': buf_putf(b, "\\n"); break;
            case '\t': buf_putf(b, "\\t"); break;
            default:
                if(*p < 0x20)
                    buf_putf(b, "\\%03o", *p);
                else
                    buf_putf(b, "%c", *p);
                break;
        }
    }

    buf_putf(b, "\"");
}

// rewritten only when different, so nothing recompiles needlessly
static void write_if_changed(const char* path, const Buf* b) {
    FILE* f = fopen(path, "rb");

    if(f) {
        bool same = false;
        char* old = xrealloc(NULL, b->len + 1);

        if(fread(old, 1, b->len + 1, f) == b->len)
            same = !memcmp(old, b->data, b->len);

        free(old);
        fclose(f);
        if(same) return;
    }

    f = fopen(path, "wb");
    if(!f || fwrite(b->data, 1, b->len, f) != b->len || fclose(f) != 0) {
        fprintf(stderr, "gen_tools: cannot write '%s'\n", path);
        exit(EXIT_FAILURE);
    }
}

// ---- checks -----------------------------------------------------------------

// lower_snake_case: the only names that become C identifiers
static bool is_ident(const char* s) {
    if(!s || !(*s >= 'a' && *s <= 'z')) return false;

    for(const char* p = s; *p; p++) {
        if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return false;
    }

    return true;
}

static bool in_list(const char* s, const char* const* list) {
    for(; list && *list; list++) {
        if(!strcmp(*list, s)) return true;
    }

    return false;
}

// "x-..." keys not in 'allowed' fail: a typo must not pass silently
static void check_extensions(yyjson_val* obj, const char* const* allowed,
                             const char* what) {
    size_t idx, max;
    yyjson_val *key, *val;

    yyjson_obj_foreach(obj, idx, max, key, val) {
        const char* k = yyjson_get_str(key);
        if(!strncmp(k, "x-", 2) && !in_list(k, allowed))
            fail("%s: unknown extension '%s'", what, k);
    }
}

// ---- enums ------------------------------------------------------------------

typedef struct Enum {
    const char* name;  // spec name, lower_snake_case
    const char* ctype; // the C type, generated or from the spec
    bool generated;    // plugin-only: the C enum is generated too
    bool used;         // an argument uses it
    size_t count;
    const char** names;   // value names
    const char** cvalues; // C expressions
} Enum;

static Enum* enums;
static size_t nenums;

static void enums_free(void) {
    for(size_t i = 0; i < nenums; i++) {
        free((void*)enums[i].names);
        free((void*)enums[i].cvalues);
    }

    free(enums);
    enums = NULL;
    nenums = 0;
}

static Enum* find_enum(const char* name) {
    for(size_t i = 0; i < nenums; i++) {
        if(!strcmp(enums[i].name, name)) return &enums[i];
    }

    return NULL;
}

static const char* enum_cvalue(const Enum* e, const char* name) {
    for(size_t i = 0; i < e->count; i++) {
        if(!strcmp(e->names[i], name)) return e->cvalues[i];
    }

    return NULL;
}

static void parse_enum(Enum* e, const char* name, yyjson_val* def) {
    e->name = name;

    if(!is_ident(name)) fail("enum '%s' must be lower_snake_case", name);
    if(find_enum(name)) fail("enum '%s' is defined twice", name);
    if(!yyjson_is_obj(def)) fail("enum '%s' must be an object", name);

    check_extensions(def, NULL, str_f("enum '%s'", name));

    yyjson_val* values = yyjson_obj_get(def, "values");
    const char* ctype = yyjson_get_str(yyjson_obj_get(def, "type"));

    e->count = yyjson_get_len(values);
    if(!e->count) fail("enum '%s' has no values", name);

    e->names = (const char**)xrealloc(NULL, e->count * sizeof(*e->names));
    e->cvalues = (const char**)xrealloc(NULL, e->count * sizeof(*e->cvalues));

    if(yyjson_is_arr(values)) {
        if(ctype) fail("enum '%s': \"type\" is only for mapped enums", name);

        char* upper = str_upper(name);
        e->generated = true;
        e->ctype = str_camel(name);

        size_t idx, max;
        yyjson_val* v;

        yyjson_arr_foreach(values, idx, max, v) {
            const char* n = yyjson_get_str(v);
            if(!is_ident(n))
                fail("enum '%s': values must be lower_snake_case", name);

            e->names[idx] = n;
            e->cvalues[idx] = str_f("MCP_%s_%s", upper, str_upper(n));
        }
    }
    else if(yyjson_is_obj(values)) {
        if(!ctype || !*ctype)
            fail("enum '%s': a mapped enum needs its C \"type\"", name);

        e->ctype = ctype;

        size_t idx, max;
        yyjson_val *k, *v;

        yyjson_obj_foreach(values, idx, max, k, v) {
            const char* n = yyjson_get_str(k);
            const char* c = yyjson_get_str(v);

            if(!is_ident(n))
                fail("enum '%s': values must be lower_snake_case", name);
            if(!c || !*c) fail("enum '%s': '%s' has no C value", name, n);

            e->names[idx] = n;
            e->cvalues[idx] = c;
        }
    }
    else {
        fail("enum '%s': \"values\" must be an array or an object", name);
    }

    for(size_t i = 0; i < e->count; i++) {
        for(size_t j = i + 1; j < e->count; j++) {
            if(!strcmp(e->names[i], e->names[j]))
                fail("enum '%s': '%s' is listed twice", name, e->names[i]);
        }
    }
}

static void parse_enums(yyjson_val* root) {
    yyjson_val* obj = yyjson_obj_get(root, "enums");
    if(!obj) return;
    if(!yyjson_is_obj(obj)) fail("\"enums\" must be an object");

    enums = xrealloc(NULL, yyjson_obj_size(obj) * sizeof(*enums));

    size_t idx, max;
    yyjson_val *key, *def;

    yyjson_obj_foreach(obj, idx, max, key, def) {
        Enum e = {0};
        parse_enum(&e, yyjson_get_str(key), def);
        enums[nenums++] = e;
    }
}

static void gen_enum(const Enum* e, Buf* h, Buf* c) {
    if(e->generated) {
        buf_putf(h, "typedef enum {\n");
        for(size_t i = 0; i < e->count; i++)
            buf_putf(h, "    %s,\n", e->cvalues[i]);
        buf_putf(h, "} %s;\n\n", e->ctype);
    }

    buf_putf(h, "const char* mcp_%s_name(%s v); // NULL if it has no name\n\n",
             e->name, e->ctype);

    // only for enums some argument uses, or it's an unused static function
    if(e->used) {
        buf_putf(c,
                 "static bool _mcp_parse_%s(const char* s, %s* out) {\n"
                 "    if(!s) return false;\n",
                 e->name, e->ctype);

        for(size_t i = 0; i < e->count; i++) {
            buf_putf(c,
                     "    if(!strcmp(s, \"%s\")) {\n"
                     "        *out = %s;\n"
                     "        return true;\n"
                     "    }\n",
                     e->names[i], e->cvalues[i]);
        }

        buf_putf(c, "    return false;\n}\n\n");
    }

    // a switch: two names for the same value fail to compile
    buf_putf(c, "const char* mcp_%s_name(%s v) {\n    switch(v) {\n", e->name,
             e->ctype);

    for(size_t i = 0; i < e->count; i++) {
        buf_putf(c, "        case %s: return \"%s\";\n", e->cvalues[i],
                 e->names[i]);
    }

    buf_putf(c, "        default: return NULL;\n    }\n}\n\n");
}

// ---- tools ------------------------------------------------------------------

typedef struct Out {
    yyjson_mut_doc* doc;  // the 'tools/list' result
    yyjson_mut_val* list; // its "tools" array
    Buf acc_h;            // argument accessors: prototypes
    Buf acc_c;            // argument accessors: bodies
    Buf executors;        // executor prototypes
    Buf table;            // dispatch table entries
} Out;

// yyjson_mut_get_int() is an 'int': this keeps all 64 bits
static long long mut_int(yyjson_mut_val* v) {
    return yyjson_mut_is_uint(v) ? (long long)yyjson_mut_get_uint(v)
                                 : (long long)yyjson_mut_get_sint(v);
}

static bool mut_in_strs(yyjson_mut_val* arr, const char* s) {
    size_t idx, max;
    yyjson_mut_val* v;

    yyjson_mut_arr_foreach(arr, idx, max, v) {
        if(yyjson_mut_equals_str(v, s)) return true;
    }

    return false;
}

// "enum" from "x-enum" and "x-only", returns the enum
static const Enum* gen_enum_arg(Out* o, const char* what, yyjson_mut_val* def,
                                bool required) {
    const char* ename = yyjson_mut_get_str(yyjson_mut_obj_get(def, "x-enum"));
    Enum* e = find_enum(ename);

    if(!e) fail("%s: unknown enum '%s'", what, ename);

    if(!yyjson_mut_equals_str(yyjson_mut_obj_get(def, "type"), "string"))
        fail("%s: an enum is a \"string\"", what);

    yyjson_mut_val* allowed = yyjson_mut_arr(o->doc);
    yyjson_mut_val* only = yyjson_mut_obj_get(def, "x-only");

    if(only) {
        size_t idx, max;
        yyjson_mut_val* v;

        yyjson_mut_arr_foreach(only, idx, max, v) {
            const char* n = yyjson_mut_get_str(v);
            if(!n || !enum_cvalue(e, n))
                fail("%s: '%s' isn't in enum '%s'", what, n ? n : "?", e->name);

            yyjson_mut_arr_add_str(o->doc, allowed, n);
        }
    }
    else {
        for(size_t i = 0; i < e->count; i++)
            yyjson_mut_arr_add_str(o->doc, allowed, e->names[i]);
    }

    yyjson_mut_obj_remove_key(def, "x-enum");
    yyjson_mut_obj_remove_key(def, "x-only");
    yyjson_mut_obj_add_val(o->doc, def, "enum", allowed);

    // an optional enum needs its default: it's the accessor's fallback
    if(!required) {
        const char* d = yyjson_mut_get_str(yyjson_mut_obj_get(def, "default"));
        if(!d) fail("%s: an optional enum needs a \"default\"", what);
        if(!mut_in_strs(allowed, d))
            fail("%s: default '%s' isn't allowed", what, d);
    }

    e->used = true;
    return e;
}

// 'fn'(args): the argument's value, its default when missing.
// The validator ran first: the value is there when required, of the right
// type, in range and one of the enum's names.
static void gen_accessor(Out* o, const char* what, const char* fn,
                         const char* arg, yyjson_mut_val* def, bool required,
                         const Enum* e) {
    const char* type = yyjson_mut_get_str(yyjson_mut_obj_get(def, "type"));
    yyjson_mut_val* d = yyjson_mut_obj_get(def, "default");
    Buf* h = &o->acc_h;
    Buf* c = &o->acc_c;

    if(e) {
        // required: the first value is never returned, the argument is there
        const char* dv =
            d ? enum_cvalue(e, yyjson_mut_get_str(d)) : e->cvalues[0];

        buf_putf(h, "%s %s(yyjson_val* args);", e->ctype, fn);
        buf_putf(c,
                 "%s %s(yyjson_val* args) {\n"
                 "    %s v = %s;\n"
                 "    _mcp_parse_%s(yyjson_get_str(yyjson_obj_get(args, "
                 "\"%s\")), &v);\n"
                 "    return v;\n"
                 "}\n\n",
                 e->ctype, fn, e->ctype, dv, e->name, arg);
    }
    else if(!strcmp(type, "string")) {
        if(d && !yyjson_mut_is_str(d))
            fail("%s: the default must be a string", what);

        buf_putf(h, "const char* %s(yyjson_val* args);", fn);
        buf_putf(c,
                 "const char* %s(yyjson_val* args) {\n"
                 "    const char* v = yyjson_get_str(yyjson_obj_get(args, "
                 "\"%s\"));\n"
                 "    return v ? v : ",
                 fn, arg);

        if(d)
            buf_put_cstr(c, yyjson_mut_get_str(d));
        else
            buf_putf(c, "NULL");

        buf_putf(c, ";\n}\n\n");
    }
    else if(!strcmp(type, "integer")) {
        if(d && !yyjson_mut_is_int(d))
            fail("%s: the default must be an integer", what);

        // positive JSON integers are stored as unsigned
        buf_putf(h, "i64 %s(yyjson_val* args);", fn);
        buf_putf(c,
                 "i64 %s(yyjson_val* args) {\n"
                 "    yyjson_val* v = yyjson_obj_get(args, \"%s\");\n"
                 "    if(!v) return %lld;\n"
                 "    return yyjson_is_uint(v) ? (i64)yyjson_get_uint(v) : "
                 "yyjson_get_sint(v);\n"
                 "}\n\n",
                 fn, arg, d ? mut_int(d) : 0LL);
    }
    else if(!strcmp(type, "boolean")) {
        if(d && !yyjson_mut_is_bool(d))
            fail("%s: the default must be a boolean", what);

        buf_putf(h, "bool %s(yyjson_val* args);", fn);
        buf_putf(c,
                 "bool %s(yyjson_val* args) {\n"
                 "    yyjson_val* v = yyjson_obj_get(args, \"%s\");\n"
                 "    return v ? yyjson_get_bool(v) : %s;\n"
                 "}\n\n",
                 fn, arg, (d && yyjson_mut_get_bool(d)) ? "true" : "false");
    }
    else {
        fail("%s: type '%s' isn't supported, use string, integer or boolean",
             what, type);
    }

    if(required)
        buf_putf(h, " // required\n");
    else if(!d)
        buf_putf(h, " // optional, no default\n");
    else if(yyjson_mut_is_str(d))
        buf_putf(h, " // default \"%s\"\n", yyjson_mut_get_str(d));
    else if(yyjson_mut_is_bool(d))
        buf_putf(h, " // default %s\n",
                 yyjson_mut_get_bool(d) ? "true" : "false");
    else
        buf_putf(h, " // default %lld\n", mut_int(d));
}

static void gen_args(Out* o, const char* tool, yyjson_mut_val* schema,
                     yyjson_val* paging) {
    yyjson_mut_val* props = yyjson_mut_obj_get(schema, "properties");
    yyjson_mut_val* required = yyjson_mut_obj_get(schema, "required");

    if(required && !yyjson_mut_is_arr(required))
        fail("tool '%s': \"required\" must be an array", tool);

    size_t idx, max;
    yyjson_mut_val* v;

    yyjson_mut_arr_foreach(required, idx, max, v) {
        const char* r = yyjson_mut_get_str(v);
        if(!r || !yyjson_mut_obj_get(props, r))
            fail("tool '%s': required '%s' isn't an argument", tool,
                 r ? r : "?");
    }

    static const char* const ARG_EXTENSIONS[] = {"x-enum", "x-only", NULL};
    yyjson_mut_val *key, *def;

    yyjson_mut_obj_foreach(props, idx, max, key, def) {
        const char* arg = yyjson_mut_get_str(key);
        const char* what = str_f("tool '%s' argument '%s'", tool, arg);
        bool req = required && mut_in_strs(required, arg);

        if(!is_ident(arg)) fail("%s: must be lower_snake_case", what);
        if(!yyjson_mut_is_obj(def)) fail("%s: must be an object", what);

        // the validator and the client both rely on these
        if(!yyjson_mut_get_str(yyjson_mut_obj_get(def, "type")))
            fail("%s: needs a \"type\"", what);
        if(!yyjson_mut_get_str(yyjson_mut_obj_get(def, "description")))
            fail("%s: needs a \"description\"", what);

        size_t i2, m2;
        yyjson_mut_val *k2, *v2;

        yyjson_mut_obj_foreach(def, i2, m2, k2, v2) {
            const char* k = yyjson_mut_get_str(k2);
            if(!strncmp(k, "x-", 2) && !in_list(k, ARG_EXTENSIONS))
                fail("%s: unknown extension '%s'", what, k);
        }

        if(req && yyjson_mut_obj_get(def, "default"))
            fail("%s: a required argument can't have a default", what);

        const Enum* e = NULL;

        if(yyjson_mut_obj_get(def, "x-enum"))
            e = gen_enum_arg(o, what, def, req);
        else if(yyjson_mut_obj_get(def, "x-only"))
            fail("%s: \"x-only\" needs \"x-enum\"", what);

        // paging arguments share one accessor, mcp_paging_<arg>()
        if(yyjson_obj_get(paging, arg)) continue;

        gen_accessor(o, what, str_f("mcp_arg_%s_%s", tool, arg), arg, def, req,
                     e);
    }
}

static void gen_tool(Out* o, yyjson_val* paging, yyjson_val* tool) {
    const char* name = yyjson_get_str(yyjson_obj_get(tool, "name"));
    const char* desc = yyjson_get_str(yyjson_obj_get(tool, "description"));

    if(!is_ident(name))
        fail("tool '%s' must be lower_snake_case", name ? name : "?");
    if(!desc || !*desc) fail("tool '%s' has no description", name);

    size_t idx, max;
    yyjson_mut_val* prev;

    yyjson_mut_arr_foreach(o->list, idx, max, prev) {
        if(yyjson_mut_equals_str(yyjson_mut_obj_get(prev, "name"), name))
            fail("tool '%s' is defined twice", name);
    }

    static const char* const TOOL_EXTENSIONS[] = {"x-context", "x-paging",
                                                  NULL};
    check_extensions(tool, TOOL_EXTENSIONS, str_f("tool '%s'", name));

    bool needs_ctx = yyjson_get_bool(yyjson_obj_get(tool, "x-context"));
    bool paged = yyjson_get_bool(yyjson_obj_get(tool, "x-paging"));

    // what the client sees: a copy without the extensions
    yyjson_mut_val* t = yyjson_val_mut_copy(o->doc, tool);
    yyjson_mut_obj_remove_key(t, "x-context");
    yyjson_mut_obj_remove_key(t, "x-paging");

    yyjson_mut_val* schema = yyjson_mut_obj_get(t, "inputSchema");

    if(!schema) {
        schema = yyjson_mut_obj_add_obj(o->doc, t, "inputSchema");
        yyjson_mut_obj_add_str(o->doc, schema, "type", "object");
    }

    yyjson_mut_val* props = yyjson_mut_obj_get(schema, "properties");
    if(!props) props = yyjson_mut_obj_add_obj(o->doc, schema, "properties");

    // the validator rejects unknown arguments: tell the client up front
    if(!yyjson_mut_obj_get(schema, "additionalProperties"))
        yyjson_mut_obj_add_bool(o->doc, schema, "additionalProperties", false);

    yyjson_val *k, *v;

    yyjson_obj_foreach(paging, idx, max, k, v) {
        if(!yyjson_mut_obj_get(props, yyjson_get_str(k))) continue;

        fail("tool '%s': '%s' is a paging argument, use x-paging", name,
             yyjson_get_str(k));
    }

    if(paged) {
        if(!paging)
            fail("tool '%s' uses x-paging but there's no \"paging\"", name);

        yyjson_obj_foreach(paging, idx, max, k, v) {
            yyjson_mut_obj_add(props, yyjson_val_mut_copy(o->doc, k),
                               yyjson_val_mut_copy(o->doc, v));
        }
    }

    gen_args(o, name, schema, paging);
    yyjson_mut_arr_append(o->list, t);

    buf_putf(&o->executors,
             "yyjson_mut_val* mcp_exec_%s(RDContext* ctx, yyjson_val* args, "
             "yyjson_mut_doc* doc);\n",
             name);

    buf_putf(&o->table, "    {\"%s\", mcp_exec_%s, %s},\n", name, name,
             needs_ctx ? "true" : "false");
}

// mcp_paging_<arg>(): shared by every tool with x-paging
static void gen_paging(Out* o, yyjson_val* paging) {
    size_t idx, max;
    yyjson_val *k, *v;

    yyjson_obj_foreach(paging, idx, max, k, v) {
        const char* arg = yyjson_get_str(k);
        const char* what = str_f("paging argument '%s'", arg);

        if(!is_ident(arg)) fail("%s: must be lower_snake_case", what);
        if(!yyjson_is_obj(v)) fail("%s: must be an object", what);

        check_extensions(v, NULL, what);
        if(!yyjson_get_str(yyjson_obj_get(v, "type")))
            fail("%s: needs a \"type\"", what);

        yyjson_mut_val* def = yyjson_val_mut_copy(o->doc, v);
        gen_accessor(o, what, str_f("mcp_paging_%s", arg), arg, def, false,
                     NULL);
    }
}

// ---- output -----------------------------------------------------------------

static const char* or_empty(const Buf* b) { return b->data ? b->data : ""; }

static void gen_files(const Out* o, const char* outdir) {
    Buf h = {0}, c = {0}, ench = {0}, encc = {0};

    for(size_t i = 0; i < nenums; i++)
        gen_enum(&enums[i], &ench, &encc);

    // minified: embedded as bytes, so nothing needs escaping
    size_t jsonlen = 0;
    char* json = yyjson_mut_write(o->doc, YYJSON_WRITE_NOFLAG, &jsonlen);
    if(!json) fail("cannot serialize the tools");

    buf_putf(&h,
             "// Generated by gen_tools from spec/tools.json: don't edit.\n"
             "#pragma once\n\n"
             "#include <redasm/redasm.h>\n"
             "#include <yyjson.h>\n\n"
             "typedef yyjson_mut_val* (*McpToolFn)(RDContext* ctx, yyjson_val* "
             "args,\n"
             "                                     yyjson_mut_doc* doc);\n\n"
             "typedef struct McpToolEntry {\n"
             "    const char* name;\n"
             "    McpToolFn run;\n"
             "    bool needs_context;\n"
             "} McpToolEntry;\n\n"
             "// same order as the tools in MCP_TOOLS_JSON\n"
             "extern const McpToolEntry MCP_TOOL_TABLE[];\n"
             "extern const usize MCP_TOOL_COUNT;\n\n"
             "// the 'tools/list' result, {\"tools\": [...]}, NUL terminated\n"
             "extern const char MCP_TOOLS_JSON[];\n"
             "extern const usize MCP_TOOLS_JSON_SIZE;\n\n"
             "// ---- enums ----\n\n"
             "%s"
             "// ---- arguments, valid once the call passed validation ----\n\n"
             "%s\n"
             "// ---- executors, written by hand in executors.c ----\n\n"
             "%s",
             or_empty(&ench), or_empty(&o->acc_h), or_empty(&o->executors));

    buf_putf(&c,
             "// Generated by gen_tools from spec/tools.json: don't edit.\n"
             "#include \"tools_gen.h\"\n"
             "#include <string.h>\n\n"
             "const McpToolEntry MCP_TOOL_TABLE[] = {\n"
             "%s"
             "};\n\n"
             "const usize MCP_TOOL_COUNT = sizeof(MCP_TOOL_TABLE) / "
             "sizeof(*MCP_TOOL_TABLE);\n\n"
             "const char MCP_TOOLS_JSON[] = {",
             or_empty(&o->table));

    for(size_t i = 0; i < jsonlen; i++)
        buf_putf(&c, "%s0x%02x,", (i % 16) ? " " : "\n    ",
                 (unsigned char)json[i]);

    buf_putf(&c,
             "\n    0x00,\n};\n\n"
             "const usize MCP_TOOLS_JSON_SIZE = %zu;\n\n"
             "// ---- enums ----\n\n"
             "%s"
             "// ---- arguments ----\n\n"
             "%s",
             jsonlen, or_empty(&encc), or_empty(&o->acc_c));

    write_if_changed(str_f("%s/tools_gen.h", outdir), &h);
    write_if_changed(str_f("%s/tools_gen.c", outdir), &c);

    free(json);
    buf_free(&encc);
    buf_free(&ench);
    buf_free(&c);
    buf_free(&h);
}

int main(int argc, char** argv) {
    if(argc != 3) {
        fputs("usage: gen_tools <tools.json> <output dir>\n", stderr);
        return EXIT_FAILURE;
    }

    spec_path = argv[1];

    yyjson_read_err err;
    yyjson_doc* doc =
        yyjson_read_file(spec_path, YYJSON_READ_NOFLAG, NULL, &err);
    if(!doc) fail("%s at byte %zu", err.msg, err.pos);

    yyjson_val* root = yyjson_doc_get_root(doc);
    if(!yyjson_is_obj(root)) fail("the spec must be an object");

    parse_enums(root);

    yyjson_val* paging = yyjson_obj_get(root, "paging");
    yyjson_val* tools = yyjson_obj_get(root, "tools");

    if(paging && !yyjson_is_obj(paging)) fail("\"paging\" must be an object");
    if(!yyjson_arr_size(tools)) fail("\"tools\" must be a non-empty array");

    Out o = {0};
    o.doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* result = yyjson_mut_obj(o.doc);
    yyjson_mut_doc_set_root(o.doc, result);
    o.list = yyjson_mut_obj_add_arr(o.doc, result, "tools");

    size_t idx, max;
    yyjson_val* tool;

    gen_paging(&o, paging);

    yyjson_arr_foreach(tools, idx, max, tool) { gen_tool(&o, paging, tool); }

    gen_files(&o, argv[2]);

    buf_free(&o.table);
    buf_free(&o.executors);
    buf_free(&o.acc_c);
    buf_free(&o.acc_h);
    yyjson_mut_doc_free(o.doc);
    enums_free();
    pool_free();
    yyjson_doc_free(doc);
    return EXIT_SUCCESS;
}
