/* =============================================================================
 * panel.cpp - Abstract panel base class: navigation & rendering
 * -----------------------------------------------------------------------------
 * Column layout in the 38-character-wide inner area (40-column panel):
 *
 *   Col.0          : mark column (reserved)
 *   Name           : left-aligned, truncated
 *   Size/<DIR>     : right-aligned
 *   Date MM-DD-YY  : right-aligned at the right edge
 *
 * Column widths are computed from the actual inner width, so the layout
 * stays correct even with a different panel width.
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "panel.h"
#include "tui.h"
#include "i18n.h"
#include "cpmap.h"
#include "umlaut.h"   /* always include last */

/* -------------------------------------------------------------------------
 * Column layout
 * ---------------------------------------------------------------------- */
struct ColLayout {
    int name_off, name_w;
    int size_off, size_w;
    int date_off, date_w;
};

static void columns(int inner, ColLayout *c)
{
    c->date_w   = 14;                               /* "DD-MM-YY HH:MM"       */
    c->date_off = inner - c->date_w;
    c->size_w   = 9;                                /* "9,999,999" / "xx.xxxk"*/
    c->size_off = c->date_off - 1 - c->size_w;
    c->name_off = 1;                                /* col.0 = mark column    */
    c->name_w   = c->size_off - 1 - c->name_off;   /* = 12, max 8.3 name     */
    if (c->name_w < 1) c->name_w = 1;               /* guard for mini panels  */
}

/* Format a file size for the narrow 9-character panel column.
 *   <= 9,999,999 : full digits grouped with the locale thousands separator
 *   10 MB ..<1 GB: binary megabytes, e.g. "10.0M" / "100M" / "954M"
 *   >= 1 GB      : binary gigabytes, e.g. "1.0G" / "4.0G"
 * M/G are binary (1024-based) units so the value matches every other size in
 * the app; the result is always <= 6 chars and never overflows the column.
 * The decimal point uses the locale separator (e.g. NL: "1,0G"). */
static void fmt_size(char *buf, unsigned long n)
{
    char raw[16];
    int  len, i, pos;

    if (n >= SZ_GB) {                              /* gigabytes */
        unsigned long whole = n / SZ_GB;
        unsigned long frac  = (n % SZ_GB) / (SZ_GB / 10UL);      /* 1st decimal */
        sprintf(buf, "%lu%c%luG", whole, g_locale.decimal_sep, frac);
        return;
    }
    if (n > 9999999UL) {                            /* megabytes */
        unsigned long mb = n / SZ_MB;
        if (mb < 100UL) {
            unsigned long frac = (n % SZ_MB) / (SZ_MB / 10UL);   /* 1st decimal */
            sprintf(buf, "%lu%c%luM", mb, g_locale.decimal_sep, frac);
        } else {
            sprintf(buf, "%luM", mb);
        }
        return;
    }

    sprintf(raw, "%lu", n);                         /* full grouped digits */
    len = (int)strlen(raw);
    pos = 0;
    for (i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) buf[pos++] = g_locale.thousands_sep;
        buf[pos++] = raw[i];
    }
    buf[pos] = '\0';
}

/* Place text in 'out' at offset 'off', left- or right-aligned within width
 * 'w'. Writes NO NUL terminator (fields sit directly next to each other). */
static void place(char *out, int off, const char *s, int w, int rightalign)
{
    int len = 0;
    int start, i;

    while (s[len] != '\0') len++;
    if (len > w) len = w;                           /* truncate              */

    start = rightalign ? (off + (w - len)) : off;
    for (i = 0; i < len; i++)
        out[start + i] = s[i];
}

/* -------------------------------------------------------------------------
 * Constructor / destructor / geometry
 * ---------------------------------------------------------------------- */
Panel::Panel()
{
    top = 1; left = 0; height = 21; width = 40;
    store = &conv;                       /* default backend: conventional memory */
    names = 0;                           /* set by the subclass                  */
    count = 0; total = 0; truncated = 0;
    cursor = 0; topentry = 0; active = 0;
    mCount = 0; mSize = 0; mDirCount = 0;
    header[0] = '\0';
    selName[0] = '\0'; atName[0] = '\0';
    s_key = SORT_NAME; s_desc = 0;
}

char *Panel::materialize(const PanelEntry *e, char *buf, int bufsz) const
{
    if (bufsz > 0) buf[0] = '\0';
    if (!names || !e || e->nameref == NAME_NONE) return 0;
    names->get(e->nameref, buf, bufsz);
    return buf[0] ? buf : 0;
}

Panel::~Panel()
{
}

/* -------------------------------------------------------------------------
 * Sorting (configurable per panel)
 * ---------------------------------------------------------------------- */
void Panel::sort_entries()
{
    store->sort(s_key, s_desc);
}

void Panel::set_sort(int key, int desc)
{
    /* Guard against out-of-range keys (e.g. the retired SORT_TIME=4 in an older
     * FTP4DOS.SAV): fall back to name order. */
    if (key < SORT_NAME || key > SORT_DATE) key = SORT_NAME;
    s_key  = (unsigned char)key;
    s_desc = (unsigned char)(desc ? 1 : 0);
}

void Panel::resort()
{
    sort_entries();
    if (cursor >= count) cursor = count ? count - 1 : 0;
    clamp_scroll();
}

void Panel::set_region(int top_, int left_, int height_, int width_)
{
    top = top_; left = left_; height = height_; width = width_;
}

unsigned char Panel::frame_attr() const
{
    return ATTR_BORDER;
}

/* Base panels (local DOS filesystem) apply the Norton case convention to
 * every plain 8.3 name; long names keep the case they carry. */
int Panel::nc_case(const char *name) const
{
    return name_is_83(name);
}

/* Is 'name' a plain 8.3 short name?
 *
 * The Norton convention (UPPERCASE directories, lowercase files) exists
 * because a FAT short name carries no case at all - DOS stores it uppercase,
 * so displaying it that way loses nothing. A long filename does carry case,
 * and forcing "My Documents" to "MY DOCUMENTS" or "Release Notes.txt" to
 * "release notes.txt" throws away information the user put there (reported on
 * VOGONS once DOSLFN support landed). So the convention now applies only to
 * names that really are 8.3; everything else is shown verbatim.
 * On plain DOS 6.22 every name is 8.3, so nothing changes there. */
int name_is_83(const char *name)
{
    int base = 0, ext = 0, dots = 0;
    const char *p;

    if (name == 0 || name[0] == '\0') return 1;
    /* "." and ".." are 8.3 by definition. */
    if (name[0] == '.' && (name[1] == '\0' ||
                          (name[1] == '.' && name[2] == '\0'))) return 1;

    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (++dots > 1) return 0;
            continue;
        }
        /* A space or LFN-only punctuation cannot occur in a short name. */
        if (c == ' ' || strchr("+,;=[]", (int)c) != 0) return 0;
        if (dots) { if (++ext  > 3) return 0; }
        else      { if (++base > 8) return 0; }
    }
    return 1;
}

/* Default actions: no-op. Subclasses override. */
int  Panel::enter_selected() { return 0; }
void Panel::go_parent()      { }

/* -------------------------------------------------------------------------
 * Navigation
 * ---------------------------------------------------------------------- */
int Panel::visible_rows() const
{
    /* Layout: border + path + separator on top, border at the bottom. */
    return (height > 5) ? (height - 5) : 0;
}

void Panel::clamp_scroll()
{
    int vr = visible_rows();

    if (count <= 0) { cursor = 0; topentry = 0; return; }

    if (cursor < 0)        cursor = 0;
    if (cursor >= count)   cursor = count - 1;

    if (cursor < topentry)            topentry = cursor;
    if (cursor >= topentry + vr)      topentry = cursor - vr + 1;
    if (topentry < 0)                 topentry = 0;
    if (topentry > count - vr)        topentry = count - vr;
    if (topentry < 0)                 topentry = 0;   /* count < vr */
}

/* Flicker-free cursor movement: only redraw the two affected rows, unless
 * scrolling occurred (in which case a full rebuild is needed). */
void Panel::move_step(int delta)
{
    int old_cursor = cursor;
    int old_top    = topentry;

    cursor += delta;
    clamp_scroll();

    if (topentry != old_top) {        /* scrolled at the edge -> redraw everything */
        draw();
        return;
    }
    if (cursor != old_cursor) {       /* only the old + new cursor row */
        draw_entry_row(old_cursor);
        draw_entry_row(cursor);
    }
}
void Panel::page_up()    { cursor -= visible_rows();clamp_scroll(); }
void Panel::page_down()  { cursor += visible_rows();clamp_scroll(); }
void Panel::move_home()  { cursor = 0;  topentry = 0; clamp_scroll(); }
void Panel::move_end()   { cursor = count - 1;        clamp_scroll(); }

PanelEntry *Panel::selected()
{
    if (cursor < 0 || cursor >= count) return 0;
    store->fetch(cursor, &selBuf);
    selBuf.fullname = materialize(&selBuf, selName, (int)sizeof(selName));
    return &selBuf;
}

PanelEntry *Panel::entry_at(int i)
{
    if (i < 0 || i >= count) return 0;
    store->fetch(i, &atBuf);
    atBuf.fullname = materialize(&atBuf, atName, (int)sizeof(atName));
    return &atBuf;
}

void Panel::select_by_name(const char *name)
{
    int i;
    if (name && name[0]) {
        for (i = 0; i < count; i++) {
            /* Match the truncated display name AND the full (pool) name, so
             * callers may pass either form for entries longer than the
             * display column (e.g. go_parent with a long directory leaf). */
            const PanelEntry *pe = store->peek(i);
            char full[NAME_STORE_MAX];
            if (stricmp(pe->name, name) == 0 ||
                (materialize(pe, full, (int)sizeof(full)) &&
                 stricmp(full, name) == 0)) {
                cursor = i;
                clamp_scroll();
                return;
            }
        }
    }
    clamp_scroll();              /* not found -> keep the cursor valid */
}

void Panel::set_cursor_index(int idx)
{
    cursor = idx;
    clamp_scroll();
}

/* --- Multi-selection ----------------------------------------------------- */
void Panel::toggle_mark()
{
    int marked_idx = cursor;
    int old_top    = topentry;
    PanelEntry *e  = selected();

    if (e && !e->is_parent) {
        int newv = e->marked ? 0 : 1;
        store->set_marked(cursor, newv);
        if (newv) {
            mCount++;
            if (e->is_dir) mDirCount++; else mSize += e->size;
        } else {
            mCount--;
            if (e->is_dir) mDirCount--; else mSize -= e->size;
        }
    }

    cursor++;                    /* like Norton Commander: move on down */
    clamp_scroll();

    if (topentry != old_top) {   /* scrolled -> redraw everything */
        draw();
        return;
    }
    /* flicker-free redraw of the (old) marked row + the new cursor row, if any */
    draw_entry_row(marked_idx);
    if (cursor != marked_idx)
        draw_entry_row(cursor);
}

void Panel::invert_marks()
{
    int i;
    reset_marks();
    for (i = 0; i < count; i++) {
        const PanelEntry *e = store->peek(i);
        if (e->is_parent) continue;
        int newv = e->marked ? 0 : 1;
        store->set_marked(i, newv);
        if (newv) {
            mCount++;
            if (e->is_dir) mDirCount++; else mSize += e->size;
        }
    }
    draw();
}

void Panel::clear_marks()
{
    int i;
    if (mCount == 0) return;     /* nothing marked - skip the full scan */
    for (i = 0; i < count; i++) store->set_marked(i, 0);
    reset_marks();
}

int Panel::marked_count() const
{
    return mCount;
}

unsigned long Panel::marked_size() const
{
    return mSize;
}

int Panel::marked_dir_count() const
{
    return mDirCount;
}

int Panel::find_entry(const char *name) const
{
    int i;
    if (name == 0 || name[0] == '\0') return -1;
    for (i = 0; i < count; i++) {
        const PanelEntry *e = store->peek(i);
        if (e->is_parent) continue;
        if (stricmp(e->name, name) == 0) return i;
    }
    return -1;
}

int Panel::has_entry(const char *name) const
{
    return find_entry(name) >= 0;
}

int Panel::find_prefix(const char *prefix, int start) const
{
    int i, n;
    if (prefix == 0 || prefix[0] == '\0') return -1;
    n = (int)strlen(prefix);
    if (start < 0) start = 0;
    for (i = start; i < count; i++) {
        const PanelEntry *e = store->peek(i);
        char full[NAME_STORE_MAX];
        const char *nm = materialize(e, full, (int)sizeof(full));
        if (strnicmp(nm ? nm : e->name, prefix, n) == 0) return i;
    }
    return -1;
}

/* Mark entry i (already known unmarked) and keep the running totals in sync. */
void Panel::mark_new(int i, int isdir, unsigned long esize)
{
    store->set_marked(i, 1);
    mCount++;
    if (isdir) mDirCount++; else mSize += esize;
}

void Panel::compare_mark(const Panel *other)
{
    int i;
    int other_empty = (other == 0 || other->count == 0);
    for (i = 0; i < count; i++) {
        const PanelEntry *e = store->peek(i);
        int           isdir;
        unsigned long esize;
        int           idx;
        if (e->is_parent || e->marked) continue;   /* already marked: leave totals alone */
        isdir = e->is_dir;
        esize = e->size;
        if (other_empty) { mark_new(i, isdir, esize); continue; }
        /* Capture before touching the other panel's store (separate ring). */
        idx = other->find_entry(e->name);   /* e->name: this ring, still valid */
        if (idx < 0)
            mark_new(i, isdir, esize);
        else if (!isdir && other->store->peek(idx)->size != esize)
            mark_new(i, isdir, esize);
    }
    draw();
}

/* -------------------------------------------------------------------------
 * Format an entry
 * ---------------------------------------------------------------------- */
void Panel::format_entry(const PanelEntry *e, const char *full,
                         char *out, int inner) const
{
    ColLayout c;
    int i;
    char tmp[16];
    char dispname[PANEL_DISPNAME_MAX];
    char convbuf[PANEL_DISPNAME_MAX];
    const char *src;

    columns(inner, &c);

    for (i = 0; i < inner; i++) out[i] = ' ';
    out[inner] = '\0';

    /* Name: Norton convention (directories UPPERCASE, files lowercase) unless
     * the panel preserves the original case (FTP panel: case-sensitive Unix)
     * or the name is a long filename, whose case carries information (see
     * name_is_83).
     * Use the full (untruncated) name when the store holds one, so a wide
     * name_w (e.g. full-screen Alt+F8 mode) can actually show more than the
     * PANEL_NAME_MAX-1 chars that fit in e->name; place() below truncates to
     * the real column width regardless. 'full' holds the raw WIRE bytes
     * (see cpmap.h) - for a UTF-8 name that is not display-safe, so convert
     * to the active codepage first, same as the short e->name already is. */
    if (full && cpmap_is_utf8(full)) {
        cpmap_utf8_to_cp(full, convbuf, sizeof(convbuf));
        src = convbuf;
    } else {
        src = full ? full : e->name;
    }
    if (nc_case(src)) {
        for (i = 0; src[i] && i < PANEL_DISPNAME_MAX - 1; i++)
            dispname[i] = (char)(e->is_dir
                                 ? toupper((unsigned char)src[i])
                                 : tolower((unsigned char)src[i]));
    } else {
        for (i = 0; src[i] && i < PANEL_DISPNAME_MAX - 1; i++)
            dispname[i] = src[i];
    }
    dispname[i] = '\0';
    place(out, c.name_off, dispname, c.name_w, 0);

    /* Size or <DIR> (right-aligned). */
    if (e->is_dir) {
        place(out, c.size_off, "<DIR>", c.size_w, 1);
    } else {
        fmt_size(tmp, e->size);
        place(out, c.size_off, tmp, c.size_w, 1);
    }

    /* Date+time (right-aligned). Left empty for the ".." entry.
     * Field order and separators follow the locale (2-digit year, 14 chars):
     * MDY "MM/DD/YY HH:MM" / DMY "DD-MM-YY HH:MM" / YMD "YY-MM-DD HH:MM". */
    if (!e->is_parent) {
        int  year  = (int)((1980 + (int)((e->date >> 9) & 0x7F)) % 100);
        int  month = (int)((e->date >> 5) & 0x0F);
        int  day   = (int)(e->date & 0x1F);
        int  hh    = (int)((e->time >> 11) & 0x1F);
        int  mm    = (int)((e->time >> 5)  & 0x3F);
        char ds    = g_locale.date_sep;
        char ts    = g_locale.time_sep;
        /* "ls -l" prints a YEAR instead of HH:MM for entries older than about
         * six months, so for those the time is simply not in the listing.
         * Printing "00:00" there invented a timestamp the server never sent
         * (an archive directory then showed 00:00 on every single row). Show
         * the 4-digit year instead - the Norton convention - and keep the
         * clock for entries that really carry one. MLSD listings always
         * carry one, so this only affects the LIST fallback. */
        char clock[8];
        if (e->no_time) sprintf(clock, "%5d", 1980 + (int)((e->date >> 9) & 0x7F));
        else            sprintf(clock, "%02d%c%02d", hh, ts, mm);
        if (g_locale.date_order == 1)            /* DMY */
            sprintf(tmp, "%02d%c%02d%c%02d %s", day, ds, month, ds, year, clock);
        else if (g_locale.date_order == 2)       /* YMD */
            sprintf(tmp, "%02d%c%02d%c%02d %s", year, ds, month, ds, day, clock);
        else                                     /* MDY */
            sprintf(tmp, "%02d%c%02d%c%02d %s", month, ds, day, ds, year, clock);
        place(out, c.date_off, tmp, c.date_w, 1);
    }
}

/* -------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------- */

/* Redraw a single entry row (entry index idx). If idx is outside the
 * visible window, nothing happens. Empty rows (idx beyond the list) are
 * cleared in panel color. Basis for the flicker-free cursor redraw and for
 * the full draw(). */
void Panel::draw_entry_row(int idx)
{
    int  inner = width - 2;
    int  vr    = visible_rows();
    int  rel   = idx - topentry;
    int  row;
    char buf[PANEL_HEADER_MAX];

    if (inner < 1) return;
    if (rel < 0 || rel >= vr) return;        /* not visible */
    row = top + 4 + rel;

    if (idx >= 0 && idx < count) {
        const PanelEntry *e = store->peek(idx);
        int is_cur = (active && idx == cursor);
        int is_mk  = e->marked;
        unsigned char a;
        if (is_cur) a = is_mk ? ATTR_MARKED_SEL : ATTR_SELECTED;
        else        a = is_mk ? ATTR_MARKED     : ATTR_PANEL;
        char full[NAME_STORE_MAX];
        fill_rect(row, left + 1, 1, inner, ' ', a);
        format_entry(e, materialize(e, full, (int)sizeof(full)), buf, inner);
        draw_text(row, left + 1, buf, a, inner);
    } else {
        fill_rect(row, left + 1, 1, inner, ' ', ATTR_PANEL);
    }
}

void Panel::draw()
{
    int inner = width - 2;
    unsigned char fa = frame_attr();
    unsigned char ha = ATTR_PANEL;   /* path header: always white on blue */
    int vr = visible_rows();
    int i;
    char buf[PANEL_HEADER_MAX];

    if (inner < 1) return;

    /* 1) Clear the whole panel area in panel color. */
    fill_rect(top, left, height, width, ' ', ATTR_PANEL);

    /* 2) Double border + divider below the path header. */
    draw_box(top, left, height, width, fa, 1);
    draw_hsep(top + 2, left, width, fa, 1);

    /* 3) Path header (row top+1), centered. If too long, show the end. */
    {
        const char far *h = header;
        const char far *p = h;
        int len = 0, pad;
        while (h[len] != '\0') len++;
        if (len > inner) { p = h + (len - inner); len = inner; }
        pad = (inner - len) / 2;
        fill_rect(top + 1, left + 1, 1, inner, ' ', ha);
        draw_text(top + 1, left + 1 + pad, p, ha, len);
    }

    /* 4) Column header (row top+3). */
    {
        ColLayout c;
        columns(inner, &c);
        for (i = 0; i < inner; i++) buf[i] = ' ';
        buf[inner] = '\0';
        place(buf, c.name_off, L("Name", "Name"), c.name_w, 0);
        place(buf, c.size_off, L("Size", "Gr" oe ss "e"), c.size_w, 1);
        place(buf, c.date_off, L("Date      Time", "Datum     Zeit"), c.date_w, 1);
        fill_rect(top + 3, left + 1, 1, inner, ' ', ATTR_COLHDR);
        draw_text(top + 3, left + 1, buf, ATTR_COLHDR, inner);
    }

    /* 5) Entries (rows top+4 .. top+height-2). */
    for (i = 0; i < vr; i++)
        draw_entry_row(topentry + i);
}
