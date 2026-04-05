/*
 * mg_assoc_tracker.c — Track objc_setAssociatedObject calls
 *
 * Two components:
 *   1. Mach-O symbol rebinding (minimal fishhook) to intercept
 *      objc_setAssociatedObject / objc_removeAssociatedObjects.
 *   2. Recording table: flat array of (object, value) pairs,
 *      protected by pthread_mutex for thread safety.
 *
 * NOT compiled under MG_APP_STORE_COMPLIANT — the rebinding uses
 * private Mach-O lazy/non-lazy symbol pointer tables.
 */

#ifdef MG_APP_STORE_COMPLIANT

/* Stubs: tracker is a no-op in App Store mode. */
#include "mg_assoc_tracker.h"

mg_assoc_tracker_t *mg_assoc_tracker_create(uint32_t cap) {
    (void)cap;
    return NULL;
}
void mg_assoc_tracker_destroy(mg_assoc_tracker_t *t) { (void)t; }
const mg_assoc_entry_t *mg_assoc_tracker_snapshot(
    const mg_assoc_tracker_t *t, uint32_t *count) {
    (void)t;
    if (count) *count = 0;
    return NULL;
}

#else /* !MG_APP_STORE_COMPLIANT */

#include "mg_assoc_tracker.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>
#include <objc/runtime.h>

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>

/* ================================================================== */
/* Recording table                                                     */
/* ================================================================== */

#define ASSOC_TABLE_GROW_FACTOR 2
#define ASSOC_MAX_ENTRIES       200000

struct mg_assoc_tracker {
    mg_assoc_entry_t *entries;
    uint32_t          count;
    uint32_t          capacity;
    pthread_mutex_t   lock;
};

/* Global singleton — needed because the hook functions are plain C
 * callbacks with no user-data parameter. */
static mg_assoc_tracker_t *g_tracker = NULL;

static void tracker_record(uintptr_t object, uintptr_t value) {
    mg_assoc_tracker_t *t = g_tracker;
    if (!t) return;

    pthread_mutex_lock(&t->lock);

    /* Remove any existing entry for the same (object, key).
     * Since we don't track the key, remove all entries for
     * (object, value) to avoid duplicates when the same
     * association is overwritten.  This is O(n) but associations
     * are typically few per object. */
    /* Actually, just append — duplicates are harmless for edge
     * injection (ref_graph deduplicates edges).  Removals are
     * handled by tracker_remove_all(). */

    if (t->count >= t->capacity) {
        if (t->capacity >= ASSOC_MAX_ENTRIES) {
            pthread_mutex_unlock(&t->lock);
            return; /* cap reached */
        }
        uint32_t new_cap = t->capacity * ASSOC_TABLE_GROW_FACTOR;
        if (new_cap > ASSOC_MAX_ENTRIES) new_cap = ASSOC_MAX_ENTRIES;
        mg_assoc_entry_t *new_entries = realloc(
            t->entries, sizeof(mg_assoc_entry_t) * new_cap);
        if (!new_entries) {
            pthread_mutex_unlock(&t->lock);
            return;
        }
        t->entries  = new_entries;
        t->capacity = new_cap;
    }

    t->entries[t->count].object = object;
    t->entries[t->count].value  = value;
    t->count++;

    pthread_mutex_unlock(&t->lock);
}

static void tracker_remove_all(uintptr_t object) {
    mg_assoc_tracker_t *t = g_tracker;
    if (!t) return;

    pthread_mutex_lock(&t->lock);
    uint32_t write = 0;
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->entries[i].object != object) {
            if (write != i)
                t->entries[write] = t->entries[i];
            write++;
        }
    }
    t->count = write;
    pthread_mutex_unlock(&t->lock);
}

/* ================================================================== */
/* Hook functions                                                      */
/* ================================================================== */

/* Original function pointers, saved before rebinding. */
static void (*orig_setAssociatedObject)(id, const void *, id, objc_AssociationPolicy)
    = NULL;
static void (*orig_removeAssociatedObjects)(id) = NULL;

static void hooked_setAssociatedObject(id object, const void *key,
                                       id value,
                                       objc_AssociationPolicy policy) {
    /* Call original first. */
    if (orig_setAssociatedObject)
        orig_setAssociatedObject(object, key, value, policy);

    /* Only track retain/copy policies (strong references). */
    if (value && (policy == OBJC_ASSOCIATION_RETAIN ||
                  policy == OBJC_ASSOCIATION_RETAIN_NONATOMIC ||
                  policy == OBJC_ASSOCIATION_COPY ||
                  policy == OBJC_ASSOCIATION_COPY_NONATOMIC)) {
        tracker_record((uintptr_t)object, (uintptr_t)value);
    }
}

static void hooked_removeAssociatedObjects(id object) {
    tracker_remove_all((uintptr_t)object);

    if (orig_removeAssociatedObjects)
        orig_removeAssociatedObjects(object);
}

/* ================================================================== */
/* Mach-O symbol rebinding (minimal fishhook)                          */
/* ================================================================== */

typedef struct {
    const char *name;
    void       *replacement;
    void      **original;
} rebind_entry_t;

/*
 * Rebind symbol pointers in one Mach-O image.
 * Walks the lazy and non-lazy symbol pointer sections.
 */
static void rebind_symbols_for_image(const struct mach_header_64 *header,
                                     intptr_t slide,
                                     const rebind_entry_t *entries,
                                     uint32_t entry_count) {
    /* Find LC_SYMTAB, LC_DYSYMTAB, and __LINKEDIT segment. */
    const struct symtab_command *symtab_cmd = NULL;
    const struct dysymtab_command *dysymtab_cmd = NULL;
    uintptr_t linkedit_base = 0;

    const uint8_t *cmd_ptr = (const uint8_t *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cmd_ptr;
        if (lc->cmd == LC_SYMTAB) {
            symtab_cmd = (const struct symtab_command *)lc;
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysymtab_cmd = (const struct dysymtab_command *)lc;
        } else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)lc;
            if (strcmp(seg->segname, SEG_LINKEDIT) == 0) {
                linkedit_base = (uintptr_t)slide + seg->vmaddr - seg->fileoff;
            }
        }
        cmd_ptr += lc->cmdsize;
    }

    if (!symtab_cmd || !dysymtab_cmd || !linkedit_base) return;

    const struct nlist_64 *symtab =
        (const struct nlist_64 *)(linkedit_base + symtab_cmd->symoff);
    const char *strtab = (const char *)(linkedit_base + symtab_cmd->stroff);
    const uint32_t *indirect_symtab =
        (const uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    /* Walk all sections looking for S_LAZY_SYMBOL_POINTERS and
     * S_NON_LAZY_SYMBOL_POINTERS. */
    cmd_ptr = (const uint8_t *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cmd_ptr;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)lc;
            const struct section_64 *sec =
                (const struct section_64 *)(seg + 1);

            for (uint32_t j = 0; j < seg->nsects; j++) {
                uint32_t sec_type = sec[j].flags & SECTION_TYPE;
                if (sec_type != S_LAZY_SYMBOL_POINTERS &&
                    sec_type != S_NON_LAZY_SYMBOL_POINTERS)
                    continue;

                uint32_t indirect_off = sec[j].reserved1;
                uintptr_t *ptrs = (uintptr_t *)((uintptr_t)slide + sec[j].addr);
                uint32_t ptr_count = (uint32_t)(sec[j].size / sizeof(uintptr_t));

                for (uint32_t k = 0; k < ptr_count; k++) {
                    uint32_t sym_idx = indirect_symtab[indirect_off + k];
                    if (sym_idx == INDIRECT_SYMBOL_ABS ||
                        sym_idx == INDIRECT_SYMBOL_LOCAL ||
                        sym_idx == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))
                        continue;

                    const char *sym_name = strtab + symtab[sym_idx].n_un.n_strx;
                    /* Mach-O symbol names have a leading underscore. */
                    if (sym_name[0] != '_') continue;

                    for (uint32_t e = 0; e < entry_count; e++) {
                        if (strcmp(sym_name + 1, entries[e].name) == 0) {
                            if (entries[e].original && *entries[e].original == NULL) {
                                *entries[e].original = (void *)ptrs[k];
                            }
                            /* __DATA_CONST may be read-only; make writable.
                             * Do NOT restore to r-- afterwards — the ObjC
                             * runtime may also need to write to this page
                             * during later class realization. */
                            vm_address_t page = (vm_address_t)&ptrs[k] & ~(vm_page_size - 1);
                            vm_protect(mach_task_self(), page, vm_page_size,
                                       0, VM_PROT_READ | VM_PROT_WRITE);
                            ptrs[k] = (uintptr_t)entries[e].replacement;
                            break;
                        }
                    }
                }
            }
        }
        cmd_ptr += lc->cmdsize;
    }
}

/*
 * Rebind symbols across all loaded images.
 * Returns 0 on success.
 */
static int rebind_symbols(const rebind_entry_t *entries, uint32_t count) {
    uint32_t image_count = _dyld_image_count();
    for (uint32_t i = 0; i < image_count; i++) {
        const struct mach_header *hdr = _dyld_get_image_header(i);
        if (!hdr || hdr->magic != MH_MAGIC_64) continue;

        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        rebind_symbols_for_image(
            (const struct mach_header_64 *)hdr, slide, entries, count);
    }
    return 0;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

mg_assoc_tracker_t *mg_assoc_tracker_create(uint32_t initial_capacity) {
    if (g_tracker) return g_tracker; /* already installed */

    if (initial_capacity == 0) initial_capacity = 4096;

    mg_assoc_tracker_t *t = calloc(1, sizeof(mg_assoc_tracker_t));
    if (!t) return NULL;

    t->entries = calloc(initial_capacity, sizeof(mg_assoc_entry_t));
    if (!t->entries) { free(t); return NULL; }
    t->capacity = initial_capacity;
    t->count    = 0;
    pthread_mutex_init(&t->lock, NULL);

    g_tracker = t;

    /* Install hooks. */
    rebind_entry_t hooks[] = {
        {
            .name        = "objc_setAssociatedObject",
            .replacement = (void *)hooked_setAssociatedObject,
            .original    = (void **)&orig_setAssociatedObject,
        },
        {
            .name        = "objc_removeAssociatedObjects",
            .replacement = (void *)hooked_removeAssociatedObjects,
            .original    = (void **)&orig_removeAssociatedObjects,
        },
    };
    rebind_symbols(hooks, 2);

    return t;
}

void mg_assoc_tracker_destroy(mg_assoc_tracker_t *t) {
    if (!t) return;

    /* Restore original symbols. */
    if (orig_setAssociatedObject) {
        rebind_entry_t restore[] = {
            {
                .name        = "objc_setAssociatedObject",
                .replacement = (void *)orig_setAssociatedObject,
                .original    = NULL,
            },
            {
                .name        = "objc_removeAssociatedObjects",
                .replacement = (void *)orig_removeAssociatedObjects,
                .original    = NULL,
            },
        };
        rebind_symbols(restore, 2);
    }

    orig_setAssociatedObject = NULL;
    orig_removeAssociatedObjects = NULL;
    g_tracker = NULL;

    pthread_mutex_destroy(&t->lock);
    free(t->entries);
    free(t);
}

const mg_assoc_entry_t *mg_assoc_tracker_snapshot(
    const mg_assoc_tracker_t *t, uint32_t *count) {
    if (!t || !count) {
        if (count) *count = 0;
        return NULL;
    }
    /* Called with threads suspended — no lock needed. */
    *count = t->count;
    return t->entries;
}

#endif /* !MG_APP_STORE_COMPLIANT */
