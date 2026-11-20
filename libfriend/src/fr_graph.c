/* fr_graph.c — the graph: its slots, its edges, and the operations that
 * rearrange them. Why the slots are a watermark and not a free list
 * threaded through the whole capacity is on Friend_Context.memories.
 *
 * This is where the fixed layout pays off. Inserting a memory takes a
 * slot off a free list and links two intrusive edge lists; it never
 * sorts, never rebuilds an index, and never asks for memory. The service
 * rebuilt its whole adjacency map on every insert because it had no
 * choice — its graph was a protobuf of pointers. */

#include "fr_state.h"

/* The watermarks go back to zero, which un-remembers every slot
 * wholesale: nothing below zero is read, so there is nothing to clear. A
 * hundred thousand record slots are forgotten without touching a page of
 * them. Generations are not reset: an id a frontend is still holding must
 * not start naming a different memory because the user imported over the
 * top of this one. */
static void fr_graph_reset(Friend_Context *fr)
{
    fr->memory_slots     = 0;
    fr->memory_count     = 0;
    fr->memory_free_list = FR_SLOT_NONE;
    fr->edge_slots       = 0;
    fr->edge_count       = 0;
    fr->edge_free_list   = FR_SLOT_NONE;
}

/* Pages for slots [0, slots). */
static b32 fr_memory_reach(Friend_Context *fr, i32 slots)
{
    return fr_pages_need(&fr->memory_pages,
                         sizeof(Fr_Memory_Record) * (size_t)slots);
}

static Fr_Memory_Record *fr_memory_fresh(Friend_Context *fr, u32 slot,
                                         u32 generation)
{
    Fr_Memory_Record *m = &fr->memories[slot];
    memset(m, 0, sizeof(*m));
    m->generation = generation ? generation : 1;
    m->occupied   = 1;
    m->next_free  = FR_SLOT_NONE;
    m->first_out  = FR_SLOT_NONE;
    m->first_in   = FR_SLOT_NONE;
    fr->memory_count += 1;
    return m;
}

/* The watermark of what has ever been written is kept apart from the
 * watermark of what is live, because a generation count is only
 * meaningful in a slot something wrote. */
static void fr_memory_note_reached(Friend_Context *fr)
{
    if (fr->memory_slots > fr->memory_slots_ever) {
        fr->memory_slots_ever = fr->memory_slots;
    }
}

static u32 fr_memory_alloc(Friend_Context *fr)
{
    u32 slot = fr->memory_free_list;
    u32 generation;

    if (slot != FR_SLOT_NONE) {
        fr->memory_free_list = fr->memories[slot].next_free;
        /* Keep the generation across the free: an id handed out before
         * this slot was last freed must stay distinguishable from this
         * one. */
        generation = fr->memories[slot].generation + 1;
    } else {
        if (fr->memory_slots >= fr->memory_capacity) return FR_SLOT_NONE;
        if (!fr_memory_reach(fr, fr->memory_slots + 1)) return FR_SLOT_NONE;
        slot = (u32)fr->memory_slots;
        /* A slot this friend has used before, in a life before a load or
         * an import, still has its generation; one that has never been
         * written has nothing to read. */
        generation = slot < (u32)fr->memory_slots_ever
                         ? fr->memories[slot].generation + 1
                         : 1;
        fr->memory_slots += 1;
        fr_memory_note_reached(fr);
    }
    fr_memory_fresh(fr, slot, generation);
    return slot;
}

static Fr_Memory_Record *fr_memory_claim(Friend_Context *fr, u32 slot)
{
    if (slot >= (u32)fr->memory_capacity) return 0;
    if (fr_memory(fr, slot)) return 0;
    if (!fr_memory_reach(fr, (i32)slot + 1)) return 0;

    /* Everything the watermark passes over on the way has to be a slot
     * the free list can believe: either it was never written, or it was
     * written in a life this one is replacing. */
    while (fr->memory_slots <= (i32)slot) {
        Fr_Memory_Record *skipped = &fr->memories[fr->memory_slots];
        if (fr->memory_slots >= fr->memory_slots_ever) skipped->generation = 0;
        skipped->occupied  = 0;
        skipped->next_free = FR_SLOT_NONE;
        fr->memory_slots += 1;
    }
    fr_memory_note_reached(fr);
    return fr_memory_fresh(fr, slot, 1);
}

static void fr_memory_free(Friend_Context *fr, u32 slot)
{
    Fr_Memory_Record *m = fr_memory(fr, slot);
    if (!m) return;
    m->occupied          = 0;
    m->next_free         = fr->memory_free_list;
    fr->memory_free_list = slot;
    fr->memory_count -= 1;
}

static u32 fr_edge_alloc(Friend_Context *fr)
{
    u32      slot = fr->edge_free_list;
    Fr_Edge *e;

    if (slot != FR_SLOT_NONE) {
        e                  = &fr->edges[slot];
        fr->edge_free_list = e->next_from;
    } else {
        if (fr->edge_slots >= fr->edge_capacity) return FR_SLOT_NONE;
        if (!fr_pages_need(&fr->edge_pages,
                           sizeof(Fr_Edge) * (size_t)(fr->edge_slots + 1))) {
            return FR_SLOT_NONE;
        }
        slot = (u32)fr->edge_slots;
        e    = &fr->edges[slot];
        fr->edge_slots += 1;
    }

    memset(e, 0, sizeof(*e));
    e->occupied  = 1;
    e->next_from = FR_SLOT_NONE;
    e->next_to   = FR_SLOT_NONE;
    fr->edge_count += 1;
    return slot;
}

static void fr_edge_free(Friend_Context *fr, u32 slot)
{
    Fr_Edge *e = fr_edge(fr, slot);
    if (!e) return;
    e->occupied        = 0;
    e->next_from       = fr->edge_free_list;
    fr->edge_free_list = slot;
    fr->edge_count -= 1;
}

static b32 fr_edge_add(Friend_Context *fr, u32 from, u32 to, u8 relation)
{
    u32      slot;
    Fr_Edge *e;

    if (from == to) return 0;
    if (!fr_memory(fr, from) || !fr_memory(fr, to)) return 0;
    slot = fr_edge_alloc(fr);
    if (slot == FR_SLOT_NONE) return 0;

    e            = &fr->edges[slot];
    e->from_slot = from;
    e->to_slot   = to;
    e->relation  = relation;
    e->next_from = fr->memories[from].first_out;
    e->next_to   = fr->memories[to].first_in;
    fr->memories[from].first_out = slot;
    fr->memories[to].first_in    = slot;
    return 1;
}

/* They cover the holes below the watermark and nothing above it: a slot
 * the friend has never reached is not a free slot, it is memory nobody
 * has asked the operating system for, and the allocator gets there by
 * moving the watermark. Running high to low means the first allocation
 * out of an empty friend is slot 0, which makes a saved state and a
 * replayed one easy to compare by eye. */
static void fr_rebuild_free_lists(Friend_Context *fr)
{
    i32 i;

    fr->memory_count     = 0;
    fr->memory_free_list = FR_SLOT_NONE;
    fr->edge_count       = 0;
    fr->edge_free_list   = FR_SLOT_NONE;

    for (i = fr->memory_slots - 1; i >= 0; --i) {
        if (fr->memories[i].occupied) {
            fr->memory_count += 1;
            continue;
        }
        fr->memories[i].next_free = fr->memory_free_list;
        fr->memory_free_list      = (u32)i;
    }
    for (i = fr->edge_slots - 1; i >= 0; --i) {
        if (fr->edges[i].occupied) {
            fr->edge_count += 1;
            continue;
        }
        fr->edges[i].next_from = fr->edge_free_list;
        fr->edge_free_list     = (u32)i;
    }
}

/* Removing a memory with k edges in and m out costs up to k*m bridges.
 * Past this the memory is a junction rather than a step, and pruning it
 * would cost more structure than it saves. */
#define FR_PRUNE_MAX_FANOUT 64

static void fr_unlink_edge(Friend_Context *fr, u32 edge)
{
    Fr_Edge *e = &fr->edges[edge];
    u32     *link;

    for (link = &fr->memories[e->from_slot].first_out; *link != FR_SLOT_NONE;
         link = &fr->edges[*link].next_from) {
        if (*link == edge) {
            *link = e->next_from;
            break;
        }
    }
    for (link = &fr->memories[e->to_slot].first_in; *link != FR_SLOT_NONE;
         link = &fr->edges[*link].next_to) {
        if (*link == edge) {
            *link = e->next_to;
            break;
        }
    }
    fr_edge_free(fr, edge);
}

/* Can `target` be reached from `start` by following connections forward?
 * The service's isMoreRecent: later in a chain means more recent. */
static b32 fr_reaches(Friend_Context *fr, Arena *arena, u32 start, u32 target)
{
    u32 *stack;
    u8  *seen;
    i32  depth = 0;

    if (start == target) return 1;
    stack = arena_push(arena, u32, (size_t)fr->memory_slots);
    seen  = arena_push(arena, u8, (size_t)fr->memory_slots);
    if (!stack || !seen) return 0;

    stack[depth++] = start;
    seen[start]    = 1;
    while (depth > 0) {
        u32 at   = stack[--depth];
        u32 edge = fr->memories[at].first_out;
        for (; edge != FR_SLOT_NONE; edge = fr->edges[edge].next_from) {
            u32 next = fr->edges[edge].to_slot;
            if (next == target) return 1;
            if (seen[next]) continue;
            seen[next]     = 1;
            stack[depth++] = next;
        }
    }
    return 0;
}

static void fr_mark_component(Friend_Context *fr, Arena *arena, u32 start,
                              u8 *seen)
{
    u32 *stack = arena_push(arena, u32, (size_t)fr->memory_slots);
    i32  depth = 0;

    if (!stack) return;
    stack[depth++] = start;
    seen[start]    = 1;
    while (depth > 0) {
        u32 at = stack[--depth];
        u32 edge;
        for (edge = fr->memories[at].first_out; edge != FR_SLOT_NONE;
             edge = fr->edges[edge].next_from) {
            u32 next = fr->edges[edge].to_slot;
            if (seen[next]) continue;
            seen[next]     = 1;
            stack[depth++] = next;
        }
        for (edge = fr->memories[at].first_in; edge != FR_SLOT_NONE;
             edge = fr->edges[edge].next_to) {
            u32 next = fr->edges[edge].from_slot;
            if (seen[next]) continue;
            seen[next]     = 1;
            stack[depth++] = next;
        }
    }
}

static void fr_connect_to_related(Friend_Context *fr, u32 new_slot, const u32 *related,
                           const u8 *relations, i32 count)
{
    Arena_Mark mark = arena_mark(&fr->scratch);
    u8         handled[FR_RELATE_FANOUT];
    i32        i, j;

    if (count > FR_RELATE_FANOUT) count = FR_RELATE_FANOUT;
    memset(handled, 0, sizeof(handled));

    /* One edge per connected component the neighbours fall into, from
     * the most recent memory of that component. Several neighbours on
     * one chain would otherwise each get an edge, and the chain would
     * stop being a chain. */
    for (i = 0; i < count; ++i) {
        u8 *seen;
        i32 most = i;

        if (handled[i]) continue;
        if (!fr_memory(fr, related[i])) continue;
        if (relations[i] == FRIEND_RELATION_NONE) continue;

        seen = arena_push(&fr->scratch, u8, (size_t)fr->memory_slots);
        if (!seen) break;
        fr_mark_component(fr, &fr->scratch, related[i], seen);

        for (j = i; j < count; ++j) {
            if (handled[j] || !seen[related[j]]) continue;
            if (relations[j] == FRIEND_RELATION_NONE) continue;
            handled[j] = 1;
            if (j == most) continue;
            /* j is more recent than most when you can get from most to
             * j by following the chain forward. */
            if (fr_reaches(fr, &fr->scratch, related[most], related[j])) {
                most = j;
            }
        }
        fr_edge_add(fr, related[most], new_slot, relations[most]);
    }
    arena_reset_to(&fr->scratch, mark);
}

static void fr_drop_memory(Friend_Context *fr, u32 slot)
{
    Fr_Memory_Record *m = fr_memory(fr, slot);
    Arena_Mark        mark;
    u32              *in;
    u32              *out;
    i32               in_count = 0, out_count = 0;
    u32               edge;
    i32               i, j;

    if (!m) return;
    mark = arena_mark(&fr->scratch);
    in   = arena_push(&fr->scratch, u32, FR_PRUNE_MAX_FANOUT);
    out  = arena_push(&fr->scratch, u32, FR_PRUNE_MAX_FANOUT);

    for (edge = m->first_in; edge != FR_SLOT_NONE && in;
         edge = fr->edges[edge].next_to) {
        if (in_count < FR_PRUNE_MAX_FANOUT) in[in_count++] = fr->edges[edge].from_slot;
    }
    for (edge = m->first_out; edge != FR_SLOT_NONE && out;
         edge = fr->edges[edge].next_from) {
        if (out_count < FR_PRUNE_MAX_FANOUT) {
            out[out_count++] = fr->edges[edge].to_slot;
        }
    }

    /* Free the edges before bridging, so the bridges can reuse the
     * slots the removal just gave back. */
    while (m->first_in != FR_SLOT_NONE) fr_unlink_edge(fr, m->first_in);
    while (m->first_out != FR_SLOT_NONE) fr_unlink_edge(fr, m->first_out);

    /* The chains that ran through it still run. The service stamped
     * these as SameTopic, which the flat recall rendering never shows
     * but which keeps the walk connected. */
    for (i = 0; i < in_count; ++i) {
        for (j = 0; j < out_count; ++j) {
            fr_edge_add(fr, in[i], out[j], FRIEND_RELATION_SAME_TOPIC);
        }
    }

    fr_memory_free(fr, slot);
    arena_reset_to(&fr->scratch, mark);
    fr_mark_dirty(fr);
}
