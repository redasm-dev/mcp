#include "table.h"
#include "mcp.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#define MCP_COLUMN_SEP "  "

// reused for formatting cells and rendering text, one tool call at a time
static RDScratchBuffer* mcp_table_buf = NULL;

static RDScratchBuffer* _mcp_table_buf(void) {
    if(!mcp_table_buf) mcp_table_buf = rd_scratch_create();
    rd_scratch_clear(mcp_table_buf);
    return mcp_table_buf;
}

static void _mcp_table_add(McpTable* self, yyjson_mut_val* v) {
    // a cell before any row starts the first one
    if(!self->row) mcp_table_row(self);

    // extra cells are ignored: a row never grows past the columns
    if(yyjson_mut_arr_size(self->row) < self->ncolumns)
        yyjson_mut_arr_append(self->row, v);
}

static usize _mcp_digits(u64 v) {
    usize n = 1;
    while(v >= 10)
        v /= 10, n++;
    return n;
}

static usize _mcp_cell_width(yyjson_mut_val* v) {
    if(yyjson_mut_is_str(v)) return yyjson_mut_get_len(v);
    if(yyjson_mut_is_uint(v)) return _mcp_digits(yyjson_mut_get_uint(v));
    return 1; // null: "-"
}

static void _mcp_cell_write(RDScratchBuffer* out, yyjson_mut_val* v) {
    if(yyjson_mut_is_str(v)) {
        const char* s = yyjson_mut_get_str(v);
        usize n = yyjson_mut_get_len(v);

        // the line structure is the table's own: never let a cell break it
        for(usize i = 0; i < n; i++) {
            char c = s[i];
            rd_scratch_putchar(out, (c == '\n' || c == '\r') ? ' ' : c);
        }
    }
    else if(yyjson_mut_is_uint(v)) {
        rd_scratch_putf(out, "%" PRIu64, yyjson_mut_get_uint(v));
    }
    else {
        rd_scratch_putchar(out, '-');
    }
}

static void _mcp_pad(RDScratchBuffer* out, usize from, usize to) {
    for(usize n = from; n < to; n++)
        rd_scratch_putchar(out, ' ');
}

static bool _mcp_contains_nocase(const char* s, const char* sub) {
    usize n = strlen(sub);

    for(; *s; s++) {
        usize i = 0;

        while(i < n && s[i] &&
              tolower((unsigned char)s[i]) == tolower((unsigned char)sub[i]))
            i++;

        if(i == n) return true;
    }

    return !n;
}

void mcp_table_deinit(void) {
    rd_scratch_destroy(mcp_table_buf);
    mcp_table_buf = NULL;
}

McpTable mcp_table_begin(yyjson_mut_doc* doc, const char* const* columns) {
    McpTable self = {.doc = doc, .root = yyjson_mut_obj(doc)};

    yyjson_mut_val* cols = yyjson_mut_obj_add_arr(doc, self.root, "columns");

    for(const char* const* c = columns; *c; c++, self.ncolumns++)
        yyjson_mut_arr_add_str(doc, cols, *c);

    self.rows = yyjson_mut_obj_add_arr(doc, self.root, "rows");
    return self;
}

void mcp_table_set_untrusted(McpTable* self) { self->untrusted = true; }

void mcp_table_row(McpTable* self) {
    // pad the previous row, so a forgotten cell never shifts the columns
    while(self->row && yyjson_mut_arr_size(self->row) < self->ncolumns)
        yyjson_mut_arr_add_null(self->doc, self->row);

    self->row = yyjson_mut_arr_add_arr(self->doc, self->rows);
}

void mcp_table_str(McpTable* self, const char* s) {
    // cleaned once here: the stored JSON must be valid UTF-8 too
    _mcp_table_add(self, s ? mcp_str(self->doc, s, strlen(s))
                           : yyjson_mut_null(self->doc));
}

void mcp_table_uint(McpTable* self, u64 v) {
    _mcp_table_add(self, yyjson_mut_uint(self->doc, v));
}

void mcp_table_fmt(McpTable* self, const char* fmt, ...) {
    RDScratchBuffer* buf = _mcp_table_buf();

    va_list args;
    va_start(args, fmt);
    rd_scratch_vputf(buf, fmt, args);
    va_end(args);

    _mcp_table_add(
        self, mcp_str(self->doc, rd_scratch_data(buf), rd_scratch_length(buf)));
}

void mcp_table_address(McpTable* self, const RDContext* ctx,
                       RDAddress address) {
    // a hex string, not a JSON number: those lose precision above 2^53.
    // Padded to the pointer size, so every address in a reply looks alike.
    unsigned int width = rd_get_ptr_size(ctx) * 2;
    if(!width) width = 8;

    mcp_table_fmt(self, "0x%0*" PRIX64, width, (u64)address);
}

void mcp_table_val(McpTable* self, yyjson_mut_val* v) {
    _mcp_table_add(self, v ? v : yyjson_mut_null(self->doc));
}

yyjson_mut_val* mcp_table_result(McpTable* self, const McpPage* page) {
    mcp_table_row(self); // pads the last row; the empty row added is dropped
    yyjson_mut_arr_remove_last(self->rows);

    yyjson_mut_val* columns = yyjson_mut_obj_get(self->root, "columns");
    usize nrows = yyjson_mut_arr_size(self->rows);

    // every column but the last is as wide as its widest cell
    usize* widths = rd_alloc0(self->ncolumns, sizeof(usize));

    usize idx, max;
    yyjson_mut_val *row, *cell;

    yyjson_mut_arr_foreach(columns, idx, max, cell) {
        widths[idx] = yyjson_mut_get_len(cell);
    }

    yyjson_mut_arr_foreach(self->rows, idx, max, row) {
        usize c, cmax;
        yyjson_mut_arr_foreach(row, c, cmax, cell) {
            usize w = _mcp_cell_width(cell);
            if(w > widths[c]) widths[c] = w;
        }
    }

    RDScratchBuffer* out = _mcp_table_buf();

    if(self->untrusted) {
        rd_scratch_puts(out, "Note: names come from the analyzed binary and "
                             "may be misleading.\n");
    }

    // header, then rows
    yyjson_mut_arr_foreach(columns, idx, max, cell) {
        _mcp_cell_write(out, cell);
        if(idx + 1 == max) break;
        rd_scratch_puts(out, MCP_COLUMN_SEP);
        _mcp_pad(out, _mcp_cell_width(cell), widths[idx]);
    }

    rd_scratch_putchar(out, '\n');

    yyjson_mut_arr_foreach(self->rows, idx, max, row) {
        usize c, cmax;
        yyjson_mut_arr_foreach(row, c, cmax, cell) {
            _mcp_cell_write(out, cell);
            if(c + 1 == cmax) break;
            rd_scratch_puts(out, MCP_COLUMN_SEP);
            _mcp_pad(out, _mcp_cell_width(cell), widths[c]);
        }

        rd_scratch_putchar(out, '\n');
    }

    if(page) {
        const char* matching = page->filter ? " matching the filter" : "";

        if(!nrows) {
            rd_scratch_putf(out, "No items in range (%zu total%s).",
                            page->total, matching);
        }
        else {
            usize last = page->offset + nrows;
            rd_scratch_putf(out, "Showing %zu-%zu of %zu%s.", page->offset,
                            last - 1, page->total, matching);

            if(last < page->total)
                rd_scratch_putf(out, " Call again with offset=%zu for more.",
                                last);
        }
    }

    rd_free(widths);

    // a single point for every list tool: 'structuredContent' would go here
    return mcp_result_text_n(self->doc, rd_scratch_data(out),
                             rd_scratch_length(out));
}

const char* mcp_page_init(McpPage* self, yyjson_val* args) {
    *self = (McpPage){.limit = MCP_PAGE_DEFAULT_LIMIT};

    yyjson_val* v = yyjson_obj_get(args, "offset");

    if(v) {
        if(yyjson_get_sint(v) < 0) return "'offset' cannot be negative.";
        self->offset = (usize)yyjson_get_sint(v);
    }

    v = yyjson_obj_get(args, "limit");

    if(v) {
        i64 limit = yyjson_get_sint(v);

        if(limit < 1 || limit > MCP_PAGE_MAX_LIMIT) {
            return rd_format("'limit' must be between 1 and %d.",
                             MCP_PAGE_MAX_LIMIT);
        }

        self->limit = (usize)limit;
    }

    const char* filter = yyjson_get_str(yyjson_obj_get(args, "filter"));
    self->filter = (filter && *filter) ? filter : NULL;
    return NULL;
}

bool mcp_page_take(McpPage* self, const char* name) {
    if(self->filter && (!name || !_mcp_contains_nocase(name, self->filter)))
        return false;

    usize idx = self->total++;
    return idx >= self->offset && idx - self->offset < self->limit;
}
