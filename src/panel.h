/* =============================================================================
 * panel.h - Abstract panel base class for NCFTP386
 * -----------------------------------------------------------------------------
 * Logic shared by the two file-list panels (local & FTP remote):
 *   - Entry list (name, size, date, directory flag)
 *   - Cursor (selected entry) + scroll offset
 *   - Keyboard navigation (up/down, paging, Home/End)
 *   - Full rendering into a screen region (Norton Commander look)
 *
 * refresh() is purely virtual: the local panel reads the DOS filesystem,
 * the FTP panel the remote LIST output. Everything else is shared here.
 *
 * Storage is delegated to an EntryStore (see entrystore.h): the default
 * ConvStore is a fixed inline array (~28 KB), so each panel object is large -
 * the link step uses -zt<n> to place it in a FAR data segment. With /EXMEM the
 * remote panel swaps in an ExtStore (XMS/EMS) for much larger listings.
 * ===========================================================================*/
#ifndef PANEL_H
#define PANEL_H

#include "namestore.h"    /* NameStore, NAME_NONE, NAME_STORE_MAX */

#define PANEL_NAME_MAX    40   /* holds local 8.3 AND longer FTP names        */
#define PANEL_MAX_ENTRIES 512  /* fixed conventional buffer (default storage) */
#define PANEL_HEADER_MAX  80   /* path/title line (DOS path max. ~64+drive)   */
/* Scratch buffer for the display name in format_entry(). Must fit the
 * widest possible name column: SCREEN_COLS (80) minus border/size/date
 * columns, so the full (untruncated, pool-backed) name can be shown in
 * full-screen (Alt+F8) mode instead of being cut off at PANEL_NAME_MAX. */
#define PANEL_DISPNAME_MAX 64

/* A directory entry (POD - re-sortable via qsort). */
struct PanelEntry {
    char          name[PANEL_NAME_MAX];
    /* Handle of the full (untruncated) name in the owning panel's NameStore,
     * or NAME_NONE when 'name' already IS the complete name. This is the form
     * that survives being stored: with /EXMEM the records themselves live in
     * XMS/EMS, where a conventional pointer would be meaningless, and the
     * name arena moved there with them (see namestore.h).
     * Sorting only ever compares 'name', so a handle is enough. */
    long          nameref;
    /* Materialized form of 'nameref', or 0. Filled in by the panel when it
     * hands an entry out through selected()/entry_at(), so entry_name() keeps
     * returning a plain string to every caller. It points into a buffer owned
     * by that panel and is therefore ONLY valid on those transient records -
     * never on a record that was appended to the store. */
    char         *fullname;
    unsigned long size;       /* size in bytes (0 for directories)            */
    unsigned      date;       /* DOS date word (bits: YYYYYYY MMMM DDDDD)     */
    unsigned      time;       /* DOS time word (bits: HHHHH MMMMMM SSSSS)     */
    unsigned char is_dir;     /* 1 = directory                                */
    unsigned char is_parent;  /* 1 = ".." entry (always sorted first)         */
    unsigned char marked;     /* 1 = marked by the user (Insert key)          */
    /* 1 = the real name did NOT fit the name pool, so 'name' holds only a cut
     * prefix and 'fullname' is 0. Such an entry must never be handed to the
     * server or the filesystem - it would address the wrong file or draw a
     * misleading 550. Every action refuses it (see name_complete() in
     * ncftp.cpp) and format_entry() marks it on screen. */
    unsigned char name_cut;
    /* 1 = the listing carried a date but no time of day (a Unix "ls -l" line
     * older than ~6 months prints the year where HH:MM would be). The time
     * column then shows the year instead of a fabricated 00:00. Always 0 for
     * MLSD listings and for local files. */
    unsigned char no_time;
};

/* The name to use when talking to the server / opening the real file: the full
 * name when present, otherwise the (short) display name. Display and sorting
 * keep using e->name directly. */
inline const char *entry_name(const PanelEntry *e)
{
    return e->fullname ? e->fullname : e->name;
}

/* 1 if 'name' is a plain 8.3 short name (which carries no case of its own, so
 * the Norton UPPER-dir/lower-file convention may be applied to it), 0 for a
 * long filename, which is displayed verbatim. Defined in panel.cpp. */
int name_is_83(const char *name);

#include "entrystore.h"   /* EntryStore / ConvStore / ExtStore (needs PanelEntry) */

class Panel {
public:
    Panel();
    virtual ~Panel();

    /* Set the screen region (top-left row/column, height, width). */
    void set_region(int top_, int left_, int height_, int width_);

    /* Active status (cursor highlight + header color). */
    void set_active(int a) { active = a; }

    /* Re-read the content - implemented by the subclass.
     * Returns: number of entries. */
    virtual int refresh() = 0;

    /* Actions (overridden by the subclass; base = no-op).
     * enter_selected(): Enter on the selected entry.
     *   Returns 1 = entered a directory (list refreshed), 0 = file/no action.
     * go_parent(): switch to the parent directory. */
    virtual int  enter_selected();
    virtual void go_parent();

    /* Draw the whole panel (border, header, columns, entries). */
    void draw();

    /* Navigation (only updates state; the caller calls draw() afterwards). */
    void page_up();
    void page_down();
    void move_home();
    void move_end();

    /* Move the cursor by delta (+1/-1) and draw FLICKER-FREE: for plain
     * movement within the visible area, only the old and new cursor row
     * are redrawn; a full rebuild only happens when scrolling at the edge. */
    void move_step(int delta);

    /* Currently selected entry (0 if the list is empty). */
    PanelEntry *selected();
    int         selected_index() const { return cursor; }
    int         entry_count()    const { return count; }

    /* Truncation status: a directory can hold more entries than fit in the
     * allocated buffer. is_truncated() is 1 then; total_count() is how many
     * entries the directory actually had. */
    int         is_truncated()  const { return truncated; }
    int         total_count()   const { return total; }

    /* --- Sorting (configurable, per panel) --- */
    /* Sort keys for set_sort(). ".." stays first and directories stay grouped
     * before files regardless of key/direction; the key orders within a group. */
    /* SORT_DATE sorts by the full timestamp (date major, time minor); there is
     * no separate time-of-day key (date+time are one entity). */
    enum { SORT_NAME = 0, SORT_EXT, SORT_SIZE, SORT_DATE };
    void set_sort(int key, int desc);   /* set the mode (does NOT re-sort)      */
    int  sort_key()  const { return s_key;  }
    int  sort_desc() const { return s_desc; }
    void resort();                      /* re-sort the current entries in place  */

    /* Set the cursor to the entry with this name (case-insensitive). If it
     * is not found, the cursor stays within the valid range. Used to "keep
     * the cursor on the item after the operation". */
    void select_by_name(const char *name);
    /* Set the cursor directly to an index (clamped to the valid range).
     * Used to "stay nearby after deleting". */
    void set_cursor_index(int idx);

    /* Entry by index (0 if outside the valid range). */
    PanelEntry *entry_at(int i);

    /* Index of the first entry at index >= start whose (full) name begins with
     * 'prefix' (case-insensitive), or -1. Used by the search / jump-to feature. */
    int find_prefix(const char *prefix, int start) const;

    /* --- Multi-selection (Norton Commander style, Insert key) --- */
    /* Toggle the mark on the current entry and move the cursor down.
     * The ".." entry cannot be marked. */
    void          toggle_mark();
    void          invert_marks();  /* numpad *: invert all marks                  */
    /* Numpad +: in the active panel, mark all entries that don't exist in
     * `other` (name, case-insensitive) or exist there with a different size
     * (files only). If `other` is empty/null: mark everything. */
    void          compare_mark(const Panel *other);
    void          clear_marks();
    int           marked_count()     const; /* number of marked entries      */
    unsigned long marked_size()      const; /* sum of sizes (files only)     */
    int           marked_dir_count() const; /* number of marked directories  */

    /* 1 if an entry with this name exists (".." excluded), case-insensitive.
     * Used for the overwrite prompt during upload. */
    int         has_entry(const char *name) const;

    /* Panel title/path line (local: directory, remote: FTP path). */
    const char *title() const { return header; }

    /* Swap in an alternative entry store (e.g. an ExtStore for /EXMEM). The
     * panel does NOT own it. Call before the first refresh(). */
    void use_store(EntryStore *s) { store = s; }

protected:
    /* Index of the entry with this name (-1 = not found, case-insensitive).
     * Internal helper for has_entry and compare_mark. */
    int         find_entry(const char *name) const;
    /* --- Screen region --- */
    int top, left, height, width;

    /* --- Content ---
     * Entries live in 'store' (ConvStore by default, ExtStore under /EXMEM).
     * 'conv' is the inline default backend; 'store' points at it unless
     * use_store() swaps in another. selBuf/atBuf back selected()/entry_at(). */
    ConvStore   conv;
    EntryStore *store;
    PanelEntry  selBuf;     /* stable buffer returned by selected()             */
    PanelEntry  atBuf;      /* stable buffer returned by entry_at()             */
    /* Full names for the two records above, so entry_name() works on them.
     * Set by the subclass to its own store (0 = no long names at all). */
    NameStore  *names;
    char        selName[NAME_STORE_MAX];
    char        atName[NAME_STORE_MAX];
    /* Copy e's full name into buf and return it, or 0 when the entry has none
     * (entry_name() then falls back to e->name). Used both for the two
     * buffers above and for the scan/draw loops, which read entries through
     * store->peek() and cannot carry a materialized pointer of their own. */
    char *materialize(const PanelEntry *e, char *buf, int bufsz) const;
    int        count;       /* number of valid entries              */
    int        total;       /* entries the directory actually had (>= count)    */
    unsigned char truncated;/* 1 = more entries existed than fit                */
    int        cursor;      /* index of the selected entry          */
    int        topentry;    /* index of the first visible entry     */
    int        active;      /* 1 = active panel                     */
    char       header[PANEL_HEADER_MAX];  /* path/title line        */

    /* --- Sort mode (per panel) --- */
    unsigned char s_key;    /* current sort key (SORT_*)            */
    unsigned char s_desc;   /* 1 = descending                       */

    /* --- Helper functions --- */
    int  visible_rows() const;   /* number of visible entry rows           */
    void clamp_scroll();         /* adjust topentry so the cursor is visible */
    void draw_entry_row(int idx);/* redraw just one entry row (idx)        */
    /* 'full' is the materialized full name (materialize(), 0 when the entry
     * has none) - the draw loops read entries through peek(), which cannot
     * carry one in the record itself. */
    void format_entry(const PanelEntry *e, const char *full,
                      char *out, int inner) const;
    /* Sort the store by the current mode (subclasses call this in refresh()). */
    void sort_entries();
    virtual unsigned char frame_attr() const;  /* border color (overridable) */
    /* Display case convention for one name: 1 = Norton style (UPPERCASE
     * directories, lowercase files), 0 = show it verbatim. The base (local)
     * panel applies it to every 8.3 name; RemotePanel narrows it further
     * because a server name may carry meaningful case (see rpanel.h). */
    virtual int nc_case(const char *name) const;
};

#endif /* PANEL_H */
