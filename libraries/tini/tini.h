/*
 * tini.h - Tiny single-header INI parser/serializer for C.
 *
 * Pure in-memory: parse from a buffer, serialize to a buffer. No file I/O.
 * In exactly one translation unit, define TINI_IMPLEMENTATION before including:
 *
 *     #define TINI_IMPLEMENTATION
 *     #include "tini.h"
 *
 * Other translation units just #include "tini.h".
 *
 *   tini* doc = tini_create(NULL);                // NULL -> libc malloc/free
 *   int err = tini_parse(doc, data, size);        // 0 ok, >0 line, -1 alloc
 *   const char* v = tini_get(doc, "section", "key");
 *   tini_set(doc, "section", "key", "value");     // NULL value removes
 *   size_t n = tini_serialize(doc, NULL);         // size needed (without NUL)
 *   char* buf = malloc(n + 1);
 *   tini_serialize(doc, buf);                     // writes n bytes + NUL
 *   tini_destroy(doc);
 *
 * Pointer-validity contract: const char* values returned by tini_get and the
 * fields of a tini_entry returned by tini_at point into the document's
 * internal arena. They remain valid until the next call to tini_set or
 * tini_parse on the same document. tini_remove_section, tini_count, tini_at,
 * tini_get, tini_serialize, and tini_destroy do not invalidate them.
 *
 * Not thread-safe. INI files larger than ~4 GiB are not supported.
 */

#ifndef TINI_H
#define TINI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* (*alloc)(size_t size, void* user);
    void  (*free)(void* ptr,    void* user);
    void* user;
} tini_allocator;

typedef struct {
    const char* section;
    const char* key;
    const char* value;
} tini_entry;

typedef struct tini tini;

tini*  tini_create(const tini_allocator* allocator);
void   tini_destroy(tini* doc);

int    tini_parse(tini* doc, const char* data, size_t size);
size_t tini_serialize(const tini* doc, char* buf);

const char* tini_get(const tini* doc, const char* section, const char* key);
int    tini_set(tini* doc, const char* section, const char* key, const char* value);
size_t tini_remove_section(tini* doc, const char* section);

size_t     tini_count(const tini* doc);
tini_entry tini_at(const tini* doc, size_t i);

#ifdef __cplusplus
}
#endif

#endif /* TINI_H */

#ifdef TINI_IMPLEMENTATION
#undef TINI_IMPLEMENTATION

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINI__BAD ((uint32_t)-1)

typedef struct {
    uint32_t section_off;
    uint32_t key_off;
    uint32_t value_off;
} tini__entry;

struct tini {
    void* (*alloc)(size_t, void*);
    void  (*free)(void*, void*);
    void*  user;

    char*    arena;
    uint32_t arena_used;
    uint32_t arena_cap;

    tini__entry* entries;
    size_t       count;
    size_t       cap;
};

static void* tini__libc_alloc(size_t size, void* user) { (void)user; return malloc(size); }
static void  tini__libc_free (void* ptr,   void* user) { (void)user; free(ptr); }

static void* tini__grow(const tini* doc, void* old, size_t old_size, size_t new_size)
{
    void* p = doc->alloc(new_size, doc->user);
    if (!p) return NULL;
    if (old_size) memcpy(p, old, old_size);
    if (old) doc->free(old, doc->user);
    return p;
}

static uint32_t tini__arena_add(tini* doc, const char* s, size_t n)
{
    if (n >= TINI__BAD) return TINI__BAD;
    uint32_t need = doc->arena_used + (uint32_t)n + 1;
    if (need < doc->arena_used) return TINI__BAD;
    if (need > doc->arena_cap) {
        uint32_t nc = doc->arena_cap ? doc->arena_cap : 64;
        while (nc < need) {
            uint32_t next = nc * 2;
            if (next <= nc) return TINI__BAD;
            nc = next;
        }
        char* p = (char*)tini__grow(doc, doc->arena, doc->arena_used, nc);
        if (!p) return TINI__BAD;
        doc->arena = p;
        doc->arena_cap = nc;
    }
    uint32_t off = doc->arena_used;
    if (n) memcpy(doc->arena + off, s, n);
    doc->arena[off + n] = '\0';
    doc->arena_used = off + (uint32_t)n + 1;
    return off;
}

static int tini__entries_grow(tini* doc)
{
    if (doc->count < doc->cap) return 0;
    size_t nc = doc->cap ? doc->cap * 2 : 8;
    if (nc < doc->cap) return -1;
    void* p = tini__grow(doc, doc->entries,
                         doc->cap * sizeof(tini__entry),
                         nc       * sizeof(tini__entry));
    if (!p) return -1;
    doc->entries = (tini__entry*)p;
    doc->cap = nc;
    return 0;
}

static size_t tini__emit(char* buf, size_t pos, const char* s, size_t n)
{
    if (buf) memcpy(buf + pos, s, n);
    return pos + n;
}

tini* tini_create(const tini_allocator* a)
{
    void* (*af)(size_t, void*) = tini__libc_alloc;
    void  (*ff)(void*, void*)  = tini__libc_free;
    void*  u = NULL;
    if (a) { af = a->alloc; ff = a->free; u = a->user; }
    tini* doc = (tini*)af(sizeof(*doc), u);
    if (!doc) return NULL;
    doc->alloc = af;
    doc->free  = ff;
    doc->user  = u;
    doc->arena = NULL;
    doc->arena_used = 0;
    doc->arena_cap  = 0;
    doc->entries = NULL;
    doc->count = 0;
    doc->cap   = 0;
    return doc;
}

void tini_destroy(tini* doc)
{
    if (!doc) return;
    void  (*ff)(void*, void*) = doc->free;
    void*  u = doc->user;
    if (doc->arena)   ff(doc->arena, u);
    if (doc->entries) ff(doc->entries, u);
    ff(doc, u);
}

size_t tini_count(const tini* doc) { return doc ? doc->count : 0; }

tini_entry tini_at(const tini* doc, size_t i)
{
    tini_entry e = { NULL, NULL, NULL };
    if (doc && i < doc->count) {
        e.section = doc->arena + doc->entries[i].section_off;
        e.key     = doc->arena + doc->entries[i].key_off;
        e.value   = doc->arena + doc->entries[i].value_off;
    }
    return e;
}

const char* tini_get(const tini* doc, const char* section, const char* key)
{
    if (!doc || !section || !key) return NULL;
    for (size_t i = 0; i < doc->count; i++) {
        if (strcmp(doc->arena + doc->entries[i].section_off, section) == 0
         && strcmp(doc->arena + doc->entries[i].key_off,     key)     == 0) {
            return doc->arena + doc->entries[i].value_off;
        }
    }
    return NULL;
}

int tini_set(tini* doc, const char* section, const char* key, const char* value)
{
    if (!doc || !section || !*section || !key || !*key) return -1;

    /* Scan once: locate any existing (section, *) for section_off reuse, plus
     * the (section, key) entry for in-place update or remove. */
    uint32_t s_off = TINI__BAD;
    for (size_t i = 0; i < doc->count; i++) {
        if (strcmp(doc->arena + doc->entries[i].section_off, section) == 0) {
            s_off = doc->entries[i].section_off;
            if (strcmp(doc->arena + doc->entries[i].key_off, key) == 0) {
                if (value == NULL) {
                    memmove(&doc->entries[i], &doc->entries[i + 1],
                            (doc->count - i - 1) * sizeof(tini__entry));
                    doc->count--;
                    return 0;
                }
                uint32_t v_off = tini__arena_add(doc, value, strlen(value));
                if (v_off == TINI__BAD) return -1;
                doc->entries[i].value_off = v_off;
                return 0;
            }
        }
    }
    if (value == NULL) return 0; /* idempotent: nothing to remove */

    if (s_off == TINI__BAD) {
        s_off = tini__arena_add(doc, section, strlen(section));
        if (s_off == TINI__BAD) return -1;
    }
    uint32_t k_off = tini__arena_add(doc, key,   strlen(key));
    if (k_off == TINI__BAD) return -1;
    uint32_t v_off = tini__arena_add(doc, value, strlen(value));
    if (v_off == TINI__BAD) return -1;
    if (tini__entries_grow(doc) < 0) return -1;

    doc->entries[doc->count].section_off = s_off;
    doc->entries[doc->count].key_off     = k_off;
    doc->entries[doc->count].value_off   = v_off;
    doc->count++;
    return 0;
}

size_t tini_remove_section(tini* doc, const char* section)
{
    if (!doc || !section) return 0;
    size_t removed = 0;
    size_t i = 0;
    while (i < doc->count) {
        if (strcmp(doc->arena + doc->entries[i].section_off, section) == 0) {
            memmove(&doc->entries[i], &doc->entries[i + 1],
                    (doc->count - i - 1) * sizeof(tini__entry));
            doc->count--;
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

int tini_parse(tini* doc, const char* data, size_t size)
{
    if (!doc) return -1;
    if (!data && size) return -1;

    uint32_t cur = TINI__BAD;
    int      line_no = 0;
    size_t   pos = 0;

    while (pos < size) {
        size_t ls = pos;
        while (pos < size && data[pos] != '\n') pos++;
        size_t le = pos;
        if (le > ls && data[le - 1] == '\r') le--;
        if (pos < size) pos++;
        line_no++;

        const char* line = data + ls;
        size_t      n    = le - ls;
        while (n && *line == ' ')        { line++; n--; }
        while (n && line[n - 1] == ' ')  { n--; }

        if (n == 0) continue;
        if (line[0] == ';' || line[0] == '#') continue;

        /* Section header: '[' first, last ']' anywhere. */
        size_t r = n;
        while (r && line[r - 1] != ']') r--;
        if (line[0] == '[' && r) {
            if (r == 2) return line_no; /* "[]" */
            cur = tini__arena_add(doc, line + 1, r - 2);
            if (cur == TINI__BAD) return -1;
            continue;
        }

        /* Earliest of '=' or ':'. */
        size_t d = 0;
        while (d < n && line[d] != '=' && line[d] != ':') d++;
        if (d == n) continue;

        if (cur == TINI__BAD) return line_no;

        const char* k  = line;
        size_t      kn = d;
        while (kn && k[kn - 1] == ' ') kn--;
        if (kn == 0) return line_no;

        const char* v  = line + d + 1;
        size_t      vn = n - d - 1;
        while (vn && *v == ' ')        { v++; vn--; }
        while (vn && v[vn - 1] == ' ') { vn--; }

        const char* sec = doc->arena + cur;
        for (size_t i = 0; i < doc->count; i++) {
            if (strcmp(doc->arena + doc->entries[i].section_off, sec) == 0) {
                const char* ek = doc->arena + doc->entries[i].key_off;
                if (strlen(ek) == kn && memcmp(ek, k, kn) == 0) return line_no;
            }
        }

        uint32_t k_off = tini__arena_add(doc, k, kn);
        if (k_off == TINI__BAD) return -1;
        uint32_t v_off = tini__arena_add(doc, v, vn);
        if (v_off == TINI__BAD) return -1;
        if (tini__entries_grow(doc) < 0) return -1;

        doc->entries[doc->count].section_off = cur;
        doc->entries[doc->count].key_off     = k_off;
        doc->entries[doc->count].value_off   = v_off;
        doc->count++;
    }
    return 0;
}

size_t tini_serialize(const tini* doc, char* buf)
{
    size_t pos = 0;
    if (doc) {
        for (size_t i = 0; i < doc->count; i++) {
            const char* s = doc->arena + doc->entries[i].section_off;
            int first = 1;
            for (size_t j = 0; j < i; j++) {
                if (strcmp(doc->arena + doc->entries[j].section_off, s) == 0) {
                    first = 0;
                    break;
                }
            }
            if (!first) continue;

            pos = tini__emit(buf, pos, "[",   1);
            pos = tini__emit(buf, pos, s,     strlen(s));
            pos = tini__emit(buf, pos, "]\n", 2);

            for (size_t j = i; j < doc->count; j++) {
                if (strcmp(doc->arena + doc->entries[j].section_off, s) != 0) continue;
                const char* k = doc->arena + doc->entries[j].key_off;
                const char* v = doc->arena + doc->entries[j].value_off;
                pos = tini__emit(buf, pos, k,    strlen(k));
                pos = tini__emit(buf, pos, " = ", 3);
                pos = tini__emit(buf, pos, v,    strlen(v));
                pos = tini__emit(buf, pos, "\n", 1);
            }
            pos = tini__emit(buf, pos, "\n", 1);
        }
    }
    if (buf) buf[pos] = '\0';
    return pos;
}

#ifdef __cplusplus
}
#endif

#endif /* TINI_IMPLEMENTATION */
