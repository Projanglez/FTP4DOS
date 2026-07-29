/* =============================================================================
 * namestore.h - Arena for the full (untruncated) names of a panel's entries
 * -----------------------------------------------------------------------------
 * A directory entry only carries PANEL_NAME_MAX-1 characters inline; anything
 * longer is interned here and referenced by PanelEntry::nameref.
 *
 * This used to be a plain conventional-memory slab reached through a char*.
 * That caps the arena at one segment (~63.5 KB), and on a machine with TSRs
 * resident far less than that is actually free - an FTP archive directory of
 * a few thousand ~55-character names then ran the arena dry and every entry
 * past it became unusable (reported on VOGONS against old-dos.ru, 2792
 * entries; only 1764 of them fit even at the full 32 KB).
 *
 * So the arena moves to the same place the entry records already live under
 * /EXMEM: XMS or EMS. A handle (byte offset) survives there, a near/far
 * pointer would not - which is why entries store a 'nameref' and the panel
 * materializes it into a conventional buffer on access.
 * ===========================================================================*/
#ifndef NAMESTORE_H
#define NAMESTORE_H

/* Longest name the store round-trips, including the terminator. 255 is the
 * DOS LFN limit and comfortably above what any FTP server sends in practice;
 * anything longer is rejected by put() and flagged as a cut name. */
#define NAME_STORE_MAX 256
/* "this entry has no interned name" - PanelEntry::name is already complete. */
#define NAME_NONE      (-1L)

class ExtMem;                       /* extmem.h */

class NameStore {
public:
    NameStore();
    ~NameStore();

    /* Reserve room for 'want' bytes.
     * allowExt != 0 permits extended/expanded memory, which is only attempted
     * when 'want' exceeds what a conventional arena could ever hold; extPref
     * is passed straight to extmem_create() (0 auto, 1 XMS, 2 EMS).
     * Backs off until something is granted, so this rarely fails outright.
     * Returns 1 if any arena was obtained, 0 if none was. */
    int  init(long want, int allowExt, int extPref);

    void reset()          { used = 0; }
    long capacity() const { return cap; }
    long in_use()   const { return used; }
    /* "XMS" / "EMS" / "conventional" - for diagnostics. */
    const char *kind() const;

    /* Intern 's'. Returns a handle for get(), or NAME_NONE when it does not
     * fit (arena full, or the name is longer than NAME_STORE_MAX-1). */
    long put(const char *s);

    /* Copy the name behind 'h' into dst (always NUL-terminated).
     * h == NAME_NONE, or no arena, yields an empty string. */
    void get(long h, char *dst, int dstsz) const;

private:
    ExtMem *mem;        /* != 0: the arena lives in XMS/EMS   */
    char   *conv;       /* != 0: the arena is conventional    */
    long    cap;
    long    used;
};

#endif /* NAMESTORE_H */
