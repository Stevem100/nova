/*
 * gc.c — per-object reclamation for Nova: a tracing mark-sweep collector.
 *
 * Every runtime object (NVValue boxes, string buffers, arrays and their
 * item lists, hash tables and their entry/order lists, environments,
 * functions, classes, instances, table key copies) is allocated from this
 * heap and carries a 16-byte header (magic, size, mark bit, type tag).
 *
 * Design notes:
 *
 * - Nova values are shared by pointer everywhere (assignment, arguments,
 *   containers, closures all alias the same NVValue), so reference
 *   counting is not viable; a tracing collector is used instead.
 *
 * - Collection runs ONLY at statement boundaries (nv_gc_maybe safe points
 *   in the statement executors). Mid-expression allocations never trigger
 *   a collection, which makes every NVValue held in a C local of the
 *   evaluator safe by construction. The few values that must survive
 *   across statement boundaries (loop iterables, call frames, callback
 *   accumulators, the value flying through a finally block, ...) are
 *   registered on an explicit pin stack (nv_gc_pin / nv_gc_unpin).
 *
 * - Roots are precise: the interpreter's builtin/global environment
 *   chains, the module cache tables, the Error class, err_value /
 *   last_expr / cur_fn and the pin stack.
 *
 * - Marking uses an explicit worklist (no C recursion on value graphs),
 *   so arbitrarily deep array/map nesting cannot overflow the stack.
 *
 * - Free memory is reused through segregated size-class bins (16..512 B)
 *   plus a first-fit list for larger blocks; adjacent free blocks are
 *   coalesced during sweep. Objects are reclaimed individually — long
 *   programs no longer grow memory without bound.
 */
#include "nova.h"

#define GC_ALIGN     ((size_t)16)
#define GC_HDRSZ     (sizeof(GCHdr))
#define GC_MIN_TOTAL (GC_HDRSZ + GC_ALIGN)
#define GC_CHUNK_MIN ((size_t)1 << 20)          /* 1 MiB slab */

/* ---------- header / payload helpers ---------- */

static GCHdr *hdr_of(void *payload) { return (GCHdr *)((char *)payload - GC_HDRSZ); }

/* the heap whose chunks are searched during the current mark phase; set
 * by nv_gc_collect() before any mark_* helper runs */
static GCHeap *mark_heap = NULL;

/* true when p lies inside one of the GC's malloc'd chunks. Value pointers
 * can also be static singletons (nil/true/false in value.c) or arena
 * nodes, which have NO block header — so the chunk range must be checked
 * before the header is dereferenced. */
static bool in_heap(void *p) {
    if (!mark_heap) return false;
    for (GCChunk *c = mark_heap->chunks; c; c = c->next) {
        char *lo = (char *)c + sizeof(GCChunk);
        char *hi = lo + c->size;
        if ((char *)p >= lo && (char *)p < hi) return true;
    }
    return false;
}

static bool is_heap(void *payload) {
    return payload && in_heap(payload) && hdr_of(payload)->magic == NV_MAGIC_ALLOC;
}

/* mark a leaf block (no children to traverse) */
static void mark_block(void *payload) {
    if (payload && in_heap(payload) && hdr_of(payload)->magic == NV_MAGIC_ALLOC)
        hdr_of(payload)->mark = 1;
}

/* ---------- allocator ---------- */

/* free-list links are stored in the (dead) payload's first pointer */
static GCHdr *fl_next(GCHdr *h) { return *(GCHdr **)((char *)h + GC_HDRSZ); }
static void   fl_set(GCHdr *h, GCHdr *next) { *(GCHdr **)((char *)h + GC_HDRSZ) = next; }

static void heap_put_free(GCHeap *g, GCHdr *h) {
    h->magic = NV_MAGIC_FREE;
    h->mark = 0;
    if (h->size <= GC_NSIZES * GC_ALIGN) {
        int idx = (int)(h->size / GC_ALIGN) - 1;
        fl_set(h, g->bins[idx]);
        g->bins[idx] = h;
    } else {
        fl_set(h, g->big);
        g->big = h;
    }
}

/* split `h`: front part (size `total`) is returned, the rest goes back to the free lists */
static GCHdr *heap_split(GCHeap *g, GCHdr *h, size_t total) {
    size_t rem = h->size - total;
    h->size = (uint32_t)total;
    GCHdr *r = (GCHdr *)((char *)h + total);
    r->magic = NV_MAGIC_FREE;
    r->size = (uint32_t)rem;
    r->mark = 0;
    r->otype = 0;
    r->aux = 0;
    heap_put_free(g, r);
    return h;
}

static GCHdr *heap_carve(GCHeap *g, size_t total) {
    if (total <= GC_NSIZES * GC_ALIGN) {
        int idx = (int)(total / GC_ALIGN) - 1;
        for (int i = idx; i < GC_NSIZES; i++) {
            GCHdr *h = g->bins[i];
            if (!h) continue;
            g->bins[i] = fl_next(h);
            if (h->size >= total + GC_MIN_TOTAL) return heap_split(g, h, total);
            return h;
        }
    }
    GCHdr **pp = &g->big;
    while (*pp) {
        if ((*pp)->size >= total) {
            GCHdr *h = *pp;
            *pp = fl_next(h);              /* unlink: pp points at the link slot */
            if (h->size >= total + GC_MIN_TOTAL) return heap_split(g, h, total);
            return h;
        }
        /* advance pp to the link slot (payload) of the current block */
        pp = (GCHdr **)((char *)*pp + GC_HDRSZ);
    }
    return NULL;
}

static GCHdr *chunk_alloc(Interp *I, size_t total) {
    GCHeap *g = &I->heap;
    GCChunk *c = g->chunks;
    if (c && c->size - c->used >= total) {
        GCHdr *h = (GCHdr *)((char *)(c + 1) + c->used);
        h->magic = NV_MAGIC_ALLOC;
        h->size = (uint32_t)total;
        h->mark = 0;
        h->otype = 0;
        h->aux = 0;
        c->used += total;
        return h;
    }
    size_t cap = GC_CHUNK_MIN;
    if (cap < total + sizeof(GCChunk)) cap = total + sizeof(GCChunk);
    GCChunk *nc = (GCChunk *)malloc(sizeof(GCChunk) + cap);
    if (!nc) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    nc->size = cap;
    nc->used = 0;
    nc->next = g->chunks;
    g->chunks = nc;
    return chunk_alloc(I, total);          /* fresh chunk always fits */
}

void *nv_gc_alloc(Interp *I, size_t n, uint8_t otype) {
    GCHeap *g = &I->heap;
    if (n == 0) n = 1;
    size_t payload = (n + GC_ALIGN - 1) & ~(GC_ALIGN - 1);
    size_t total = GC_HDRSZ + payload;
    GCHdr *h = heap_carve(g, total);
    if (!h) h = chunk_alloc(I, total);
    h->magic = NV_MAGIC_ALLOC;
    h->mark = 0;
    h->otype = otype;
    h->aux = 0;
    /* account the TRUE block span (may exceed the request when a larger
     * free block is reused whole) so sweep arithmetic stays exact */
    g->live_bytes += h->size;
    g->live_objs++;
    g->total_alloc_bytes += h->size;
    g->since_gc += h->size;
    return (char *)h + GC_HDRSZ;
}

/* ---------- marking ---------- */

typedef struct { NVValue **items; size_t n, cap; } MarkList;

static void ml_push(MarkList *ml, NVValue *v) {
    if (ml->n + 1 > ml->cap) {
        size_t cap = ml->cap ? ml->cap * 2 : 64;
        NVValue **ni = (NVValue **)realloc(ml->items, cap * sizeof(NVValue *));
        if (!ni) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        ml->items = ni;
        ml->cap = cap;
    }
    ml->items[ml->n++] = v;
}

static void mark_table_children(MarkList *ml, NVTable *t);

/* forward: mark a value; its children are queued, not recursed */
static void mark_val(MarkList *ml, NVValue *v);

/* iteratively mark an environment chain and its variables */
static void mark_env(MarkList *ml, Env *e) {
    for (; e; e = e->parent) {
        if (!is_heap(e)) break;
        GCHdr *h = hdr_of(e);
        if (h->mark) break;                /* this chain was already marked */
        h->mark = 1;
        mark_block(e->vars);
        mark_table_children(ml, e->vars);
    }
}

/* iteratively mark a class chain and its method tables */
static void mark_class_chain(MarkList *ml, ObjClass *c) {
    for (; c; c = c->base) {
        if (!is_heap(c)) break;
        GCHdr *h = hdr_of(c);
        if (h->mark) break;
        h->mark = 1;
        mark_block(c->methods);
        mark_table_children(ml, c->methods);
    }
}

static void mark_fn_obj(MarkList *ml, ObjFn *f) {
    if (!f || !is_heap(f)) return;
    GCHdr *h = hdr_of(f);
    if (h->mark) return;
    h->mark = 1;
    mark_env(ml, f->closure);              /* body/pnames/names are arena-owned */
}

/* mark a table's block list and queue all its entries' values */
static void mark_table_children(MarkList *ml, NVTable *t) {
    if (!t) return;
    mark_block(t);                         /* the NVTable struct block itself */
    mark_block(t->ents);
    mark_block(t->order);
    for (size_t k = 0; k < t->on; k++) {
        TEntry *e = &t->ents[t->order[k]];
        if (e->state != 1) continue;
        mark_block((void *)e->key);        /* key copy block owned by the table */
        mark_val(ml, e->val);              /* handles heap values and static singletons */
    }
}

/* mark one value; its children are processed later by the drain loop */
static void mark_val(MarkList *ml, NVValue *v) {
    if (!v || !is_heap(v)) return;         /* static nil/true/false singletons */
    GCHdr *h = hdr_of(v);
    if (h->mark) return;
    h->mark = 1;
    ml_push(ml, v);
}

/* handle the children of an already-marked value */
static void process_val(MarkList *ml, NVValue *v) {
    switch (v->type) {
        case T_STR:
            mark_block(v->as.s);           /* string buffer block (leaf) */
            return;
        case T_ARR: {
            NVArray *a = v->as.arr;
            mark_block(a);
            mark_block(a->items);
            for (size_t i = 0; i < a->n; i++) mark_val(ml, a->items[i]);
            return;
        }
        case T_MAP:
            mark_table_children(ml, v->as.map);
            return;
        case T_FN:
            mark_fn_obj(ml, v->as.fn);
            return;
        case T_CLASS:
            mark_class_chain(ml, v->as.cls);
            return;
        case T_INSTANCE: {
            ObjInst *o = v->as.inst;
            mark_block(o);
            mark_block(o->fields);
            mark_table_children(ml, o->fields);
            mark_class_chain(ml, o->cls);
            return;
        }
        default:
            return;                        /* nil/bool/int/float: leaves */
    }
}

/* ---------- sweeping ---------- */

static void heap_sweep(GCHeap *g) {
    for (int i = 0; i < GC_NSIZES; i++) g->bins[i] = NULL;
    g->big = NULL;

    size_t freed_bytes = 0;          /* bytes of *allocated* blocks reclaimed */
    size_t live_bytes_seen = 0;      /* bytes of live blocks, per this walk */
    unsigned long freed_objs = 0, live_objs_seen = 0;

    for (GCChunk *c = g->chunks; c; c = c->next) {
        char *p = (char *)(c + 1);
        char *end = p + c->used;
        while (p < end) {
            GCHdr *h = (GCHdr *)p;
            if (h->magic != NV_MAGIC_ALLOC && h->magic != NV_MAGIC_FREE) {
                fprintf(stderr, "nova: gc heap corruption detected\n");
                exit(1);
            }
            if (h->magic == NV_MAGIC_ALLOC && h->mark) {
                live_objs_seen++;
                live_bytes_seen += h->size;
                h->mark = 0;               /* live: keep, clear for next GC */
                p += h->size;
                continue;
            }
            /* dead (allocated) or previously-freed: coalesce the run of
             * consecutive dead blocks into one free block */
            size_t run = h->size;
            size_t dead_alloc = (h->magic == NV_MAGIC_ALLOC) ? h->size : 0;
            unsigned long nblk = (h->magic == NV_MAGIC_ALLOC) ? 1 : 0;
            char *q = p + h->size;
            while (q < end) {
                GCHdr *h2 = (GCHdr *)q;
                if (h2->magic == NV_MAGIC_ALLOC && h2->mark) break;
                if (h2->magic != NV_MAGIC_ALLOC && h2->magic != NV_MAGIC_FREE) {
                    fprintf(stderr, "nova: gc heap corruption detected\n");
                    exit(1);
                }
                if (h2->magic == NV_MAGIC_ALLOC) { nblk++; dead_alloc += h2->size; }
                run += h2->size;
                q += h2->size;
            }
            GCHdr *fh = (GCHdr *)p;
            fh->size = (uint32_t)run;
            fh->otype = 0;
            heap_put_free(g, fh);
            freed_bytes += dead_alloc;   /* stats count newly reclaimed bytes */
            freed_objs += nblk;
            p += run;
        }
        /* invariant: the block walk must consume the chunk exactly */
        if (p != end) {
            fprintf(stderr, "nova: gc heap walk overrun (chunk %p used=%zu walk=%td)\n",
                    (void *)c, c->used, (ptrdiff_t)(p - (char *)(c + 1)));
            exit(1);
        }
    }

    /* the walk visited every block: adopt the exact live totals so the
     * counters can never drift from physical reality */
    g->live_objs = live_objs_seen;
    g->live_bytes = live_bytes_seen;
    g->last_freed_bytes = freed_bytes;
    g->last_freed_objs = freed_objs;
    g->total_freed_bytes += freed_bytes;
    g->total_freed_objs += freed_objs;
}

/* ---------- collection ---------- */

void nv_gc_collect(Interp *I) {
    GCHeap *g = &I->heap;
    mark_heap = g;                     /* mark_* helpers range-check against it */
    MarkList ml;
    ml.items = NULL; ml.n = 0; ml.cap = 0;

    /* precise roots */
    mark_env(&ml, I->builtins_env);
    mark_env(&ml, I->globals);
    mark_block(I->mod_cache);
    mark_table_children(&ml, I->mod_cache);
    mark_block(I->mod_busy);
    mark_table_children(&ml, I->mod_busy);
    if (I->error_class) mark_class_chain(&ml, I->error_class);
    mark_val(&ml, I->err_value);
    mark_val(&ml, I->last_expr);
    if (I->cur_fn) mark_fn_obj(&ml, I->cur_fn);
    for (int i = 0; i < I->npins; i++) {
        if (I->pins[i].tag == GC_TAG_VALUE)
            mark_val(&ml, (NVValue *)I->pins[i].p);
        else if (I->pins[i].tag == GC_TAG_ENV)
            mark_env(&ml, (Env *)I->pins[i].p);
    }

    /* drain the worklist (explicit stack: no C recursion on value graphs) */
    while (ml.n > 0) {
        NVValue *v = ml.items[--ml.n];
        process_val(&ml, v);
    }
    free(ml.items);

    heap_sweep(g);

    g->runs++;
    g->since_gc = 0;
    g->trigger = g->live_bytes + (1u << 20);
    if (g->trigger < (1u << 20)) g->trigger = 1u << 20;
}

void nv_gc_maybe(Interp *I) {
    if (I->heap.trigger > 0 && I->heap.since_gc >= I->heap.trigger)
        nv_gc_collect(I);
}

/* ---------- pin stack ---------- */

size_t nv_gc_pin(Interp *I, void *p, int tag) {
    if (I->npins + 1 > I->pin_cap) {
        int cap = I->pin_cap ? I->pin_cap * 2 : 16;
        GCPin *np = (GCPin *)realloc(I->pins, (size_t)cap * sizeof(GCPin));
        if (!np) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        I->pins = np;
        I->pin_cap = cap;
    }
    I->pins[I->npins].p = p;
    I->pins[I->npins].tag = tag;
    return (size_t)I->npins++;
}

void nv_gc_unpin(Interp *I, size_t mark) {
    if ((int)mark <= I->npins) I->npins = (int)mark;
}

void nv_gc_repin(Interp *I, size_t mark, void *p) {
    if ((int)mark < I->npins) I->pins[mark].p = p;
}

/* ---------- shutdown ---------- */

void nv_gc_free_all(Interp *I) {
    GCHeap *g = &I->heap;
    GCChunk *c = g->chunks;
    while (c) { GCChunk *nx = c->next; free(c); c = nx; }
    g->chunks = NULL;
    for (int i = 0; i < GC_NSIZES; i++) g->bins[i] = NULL;
    g->big = NULL;
    g->live_bytes = 0;
    g->live_objs = 0;
    g->since_gc = 0;
    free(I->pins);
    I->pins = NULL;
    I->npins = 0;
    I->pin_cap = 0;
}

/* compile-time sanity: header and chunk header must be block-aligned */
typedef char gc_hdr_align_check[(sizeof(GCHdr) % GC_ALIGN == 0) ? 1 : -1];
typedef char gc_chunk_align_check[(sizeof(GCChunk) % GC_ALIGN == 0) ? 1 : -1];
