/* =============================================================================
 * namestore.cpp - Arena for the full (untruncated) names of a panel's entries
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <stdlib.h>    /* malloc, free           */
#include <string.h>    /* strlen, memcpy         */

#include "namestore.h"
#include "extmem.h"

/* Conventional fallback sizes, largest first. Halving from the top would jump
 * straight past 32 KB and so never try it, which once left the panel with no
 * arena at all on a DOS 7.1 box with SMARTDRV/DOSLFN resident. Sizes below
 * 32 KB are worse than what v1.0.0 shipped but still far better than nothing. */
static const unsigned convSizes[] = {
    65024U, 49152U, 32768U, 16384U, 8192U, 4096U
};
#define CONV_SIZES_N (int)(sizeof(convSizes) / sizeof(convSizes[0]))
#define CONV_MAX     65024U     /* 63.5 KB: just inside one segment           */
#define CONV_PREFER  32768U     /* what shipped before = the preferred floor  */
/* Headroom a conventional arena must leave behind: the data connection still
 * allocates its receive buffer (up to 16 KB) and the file transfer buffer (up
 * to 32 KB) AFTER a listing has been read, so the arena must not swallow the
 * last usable far-heap block. */
#define CONV_RESERVE 49152U

NameStore::NameStore()
{
    mem  = 0;
    conv = 0;
    cap  = 0;
    used = 0;
}

NameStore::~NameStore()
{
    if (conv) free(conv);
    if (mem)  delete mem;
}

const char *NameStore::kind() const
{
    if (mem)  return mem->kind();
    if (conv) return "conventional";
    return "none";
}

int NameStore::init(long want, int allowExt, int extPref)
{
    int i;

    if (cap > 0) return 1;                    /* already initialized */
    if (want < (long)CONV_PREFER) want = (long)CONV_PREFER;

    /* Extended/expanded memory, but only when a conventional arena could not
     * hold the target anyway: for an ordinary 512-entry listing the plain
     * malloc path is simpler, needs no XMS handle and behaves exactly as it
     * always has. */
    if (allowExt && want > (long)CONV_MAX) {
        mem = extmem_create(extPref);
        if (mem) {
            cap = mem->alloc(want);
            if (cap > 0) return 1;
            delete mem;
            mem = 0;
        }
    }

    /* Conventional arena: take the largest candidate that both fits and still
     * leaves the transfer buffers room to be allocated later. */
    for (i = 0; i < CONV_SIZES_N; i++) {
        unsigned n = convSizes[i];
        char *p;
        if ((long)n > want && want < (long)CONV_MAX) continue;  /* smaller target */
        p = (char *)malloc(n);
        if (!p) continue;
        if (n > CONV_PREFER) {
            void *probe = malloc(CONV_RESERVE);
            if (!probe) { free(p); continue; }
            free(probe);
        }
        conv = p;
        cap  = (long)n;
        return 1;
    }

    cap = 0;
    return 0;
}

long NameStore::put(const char *s)
{
    long  h;
    unsigned len;

    if (!s || cap <= 0) return NAME_NONE;
    len = (unsigned)strlen(s) + 1;
    if (len > NAME_STORE_MAX) return NAME_NONE;
    /* One byte of slack: XMS moves must be an even number of bytes, so
     * XmsMem::read() may fetch one byte past the end of the string. Keeping a
     * spare byte inside the arena means that read can never run past the
     * granted block (which XMS would reject, handing back stale data). */
    if (used + (long)len + 1L > cap) return NAME_NONE;

    h = used;
    if (mem) mem->write(h, s, (int)len);
    else     memcpy(conv + h, s, len);
    used += (long)len;
    return h;
}

void NameStore::get(long h, char *dst, int dstsz) const
{
    int n;

    if (dstsz <= 0) return;
    dst[0] = '\0';
    /* Only handles that put() actually returned are valid. Bounding against
     * 'used' rather than 'cap' matters: a PanelEntry that was zero-filled
     * instead of initialized carries nameref 0, which IS a legal offset - it
     * would otherwise read whatever the arena held there and render it as the
     * entry's name (the synthesized ".." row did exactly that once). */
    if (h < 0 || h >= used || cap <= 0) return;

    n = dstsz - 1;
    if ((long)n > cap - h - 1L) n = (int)(cap - h - 1L);   /* see put() */
    if (n <= 0) return;

    if (mem) mem->read(h, dst, n);
    else     memcpy(dst, conv + h, (unsigned)n);
    /* put() always stored the terminator; this only guards a truncated read. */
    dst[n] = '\0';
}
