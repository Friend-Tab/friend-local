/* fr_import.c — reading the friend out of the official export.
 *
 * The export is protojson of FriendState with the images inlined, which
 * means there is no protobuf here: it is a JSON document with a schema we
 * know. We walk it once and write the records as we go.
 *
 * The document is read in place. Nothing is copied except what ends up in
 * a record, so a mapped gigabyte costs us the mapping and a few megabytes
 * of scratch — which is the reason the parser is a pull parser and the
 * reason this function takes the bytes rather than a path.
 *
 * Two things in the file refer backwards: connections name memories by the
 * id strings the service used, and the active chat follows the history it
 * came out of. Rather than demand an order from the document or build an
 * index of the whole thing, we note where those arrays start and come back
 * to them. A saved offset into a document we already hold is free.
 *
 * Photographs come across into the blob file's staging, decoded straight
 * out of the document, and are written out through the I/O pump after
 * the call returns. One the friend has nowhere to put — no blob_path, or
 * no room left in staging — is described and not kept.
 *
 * What does not come across:
 *
 *   - The overheard queue: audio that had been transcribed but not yet
 *     folded into the chat when the export was taken. It is seconds of
 *     ambient noise in the balance, and taking it would mean running the
 *     ambient pipeline before the friend has said hello.
 *   - Speaker-similarity judgments, which were a diarization service we
 *     cannot host. Transcriptions come across with the hedged framing the
 *     service used for everything it was unsure of.
 */

#include "../../base/fr_json.h"
#include "fr_state.h"

/* The export's field names are lowerCamelCase, but protojson accepts the
 * proto's own snake_case too and a hand-edited file may well use it. One
 * comparison that ignores case and underscores covers both without a
 * table of aliases. */
static b32 fr_field_is(String key, const char *name)
{
    i32         i = 0;
    const char *n = name;
    for (;;) {
        while (i < key.len && key.data[i] == '_') ++i;
        while (*n == '_') ++n;
        if (i >= key.len || *n == 0) break;
        if (fr_lower_char(key.data[i]) != fr_lower_char(*n)) return 0;
        ++i;
        ++n;
    }
    return i >= key.len && *n == 0;
}

/* ------------------------------------------------------------------ *
 *  id -> slot
 *
 *  The service's graph was keyed by uuid strings and it rebuilt a hash map
 *  of them on every mutation. Here the ids are needed exactly once, to
 *  turn the connection list into slot indices, so the table is scratch
 *  that dies with the import. The keys are views into the document: no
 *  copy, and an exact comparison rather than trusting a hash.
 * ------------------------------------------------------------------ */

typedef struct Fr_Id_Table {
    String *ids;
    u32    *slots;
    u32     mask;
} Fr_Id_Table;

static u64 fr_hash_string(String s)
{
    u64 h = 0xcbf29ce484222325u; /* FNV-1a */
    i32 i;
    for (i = 0; i < s.len; ++i) {
        h ^= (u64)(u8)s.data[i];
        h *= 0x100000001b3u;
    }
    return h;
}

static b32 fr_id_table_make(Fr_Id_Table *t, Arena *arena, i32 wanted)
{
    u32 capacity = 16;
    while (capacity < (u32)wanted * 2u && capacity < 0x40000000u) capacity <<= 1;
    t->ids   = arena_push(arena, String, capacity);
    t->slots = arena_push(arena, u32, capacity);
    t->mask  = capacity - 1;
    return t->ids != 0 && t->slots != 0;
}

static void fr_id_put(Fr_Id_Table *t, String id, u32 slot)
{
    u32 at = (u32)fr_hash_string(id) & t->mask;
    if (id.len <= 0) return;
    while (t->ids[at].len != 0) {
        if (str_eq(t->ids[at], id)) break; /* a duplicate id: last one wins */
        at = (at + 1) & t->mask;
    }
    t->ids[at]   = id;
    t->slots[at] = slot;
}

static u32 fr_id_get(const Fr_Id_Table *t, String id)
{
    u32 at = (u32)fr_hash_string(id) & t->mask;
    if (id.len <= 0) return FR_SLOT_NONE;
    while (t->ids[at].len != 0) {
        if (str_eq(t->ids[at], id)) return t->slots[at];
        at = (at + 1) & t->mask;
    }
    return FR_SLOT_NONE;
}

/* ------------------------------------------------------------------ *
 *  Import state
 * ------------------------------------------------------------------ */

typedef struct Fr_Import {
    Friend_Context *fr;
    String          document;

    /* Two arenas, not one: unescaping a field pushes, and assembling a
     * string from several fields holds the tail of an arena. They cannot
     * be the same arena, and saying so here is cheaper than finding out
     * later. Both are reset for every record. */
    Arena strings;
    Arena build;

    Fr_Id_Table          ids;
    Friend_Import_Report report;
    b32                  reuse_embeddings;
} Fr_Import;

static void fr_import_record_begin(Fr_Import *im)
{
    arena_reset(&im->strings);
    arena_reset(&im->build);
}

/* ------------------------------------------------------------------ *
 *  A ChatEvent
 *
 *  Read once into this, because the same shape appears in three places:
 *  the active chat, the message history, and the related events hanging
 *  off a memory.
 * ------------------------------------------------------------------ */

typedef struct Fr_Imported_Event {
    u64                 at_unix_ms;
    Friend_Message_Kind kind;
    Fr_Turn_Kind        turn;
    b32                 was_transcribed;
    f32                 interrupted_fraction;
    String              text;

    /* Captions and images are parallel arrays in the export and stay
     * parallel here. The photographs are kept as the base64 they are in
     * the document — views into it, not copies — and decoded straight
     * into the blob file's staging when the record is written. */
    String captions[FR_MEMORY_IMAGES_MAX];
    String images[FR_MEMORY_IMAGES_MAX];
    String thumbs[FR_MEMORY_IMAGES_MAX];
    i32    caption_count;
    i32    image_count;
    i32    thumb_count;
} Fr_Imported_Event;

/* A segment's deprecated word list. Long past the point where a segment
 * is a sentence, so a cap here costs nothing real. */
#define FR_SEGMENT_WORDS_MAX 64

/* Similarity, in the service's words. The judgment itself is gone with
 * the diarization service, but imported transcriptions keep the framing
 * they were shown under or the model would read them as certainties. */
static const char *fr_similarity_text(i64 value, String name)
{
    if (value == 1 || str_eq(name, str_lit("SIMILARITY_NOT_AT_ALL"))) {
        return "not at all like the user. This is probably not the user";
    }
    if (value == 2 || str_eq(name, str_lit("SIMILARITY_A_LITTLE"))) {
        return "a little like the user";
    }
    if (value == 3 || str_eq(name, str_lit("SIMILARITY_SOMEWHAT")) ||
        value == 4 || str_eq(name, str_lit("SIMILARITY_A_LOT")) ||
        value == 5 || str_eq(name, str_lit("SIMILARITY_EXACTLY"))) {
        /* The service capped confidence here on purpose: it was too sure
         * of itself about who was speaking. */
        return "somewhat like the user, could be the user could not be the "
               "user";
    }
    return "somewhat like the user";
}

/* One segment of a diarized transcription, rendered the way the service
 * rendered it into the chat. */
static void fr_read_transcription(Fr_Import *im, Json_Parser *p,
                                  String_Builder *sb)
{
    String key;
    json_object_begin(p);
    while (json_object_next(p, &key)) {
        if (!fr_field_is(key, "segments")) {
            json_skip(p);
            continue;
        }
        json_array_begin(p);
        while (json_array_next(p)) {
            String     text       = str_nil;
            String     enum_name  = str_nil;
            i64        similarity = 0;
            String     words[FR_SEGMENT_WORDS_MAX];
            i32        word_count = 0;
            String     inner;
            Arena_Mark mark = arena_mark(&im->strings);

            json_object_begin(p);
            while (json_object_next(p, &inner)) {
                if (fr_field_is(inner, "text")) {
                    text = json_string(p, &im->strings);
                } else if (fr_field_is(inner, "words")) {
                    /* Deprecated, and shown space-separated when present.
                     * Kept as views rather than joined into a buffer: sb
                     * owns the tail of build, so a second builder on it
                     * would quietly interleave with this one. */
                    json_array_begin(p);
                    while (json_array_next(p)) {
                        String word = json_string(p, &im->strings);
                        if (word_count < FR_SEGMENT_WORDS_MAX) {
                            words[word_count] = word;
                        }
                        ++word_count;
                    }
                    if (word_count > FR_SEGMENT_WORDS_MAX) {
                        word_count = FR_SEGMENT_WORDS_MAX;
                    }
                } else if (fr_field_is(inner, "similarity")) {
                    if (json_peek(p) == JSON_STRING) {
                        enum_name = json_string_raw(p);
                    } else {
                        similarity = json_i64(p);
                    }
                } else {
                    json_skip(p);
                }
            }

            sb_append_cstr(sb, "This transcribed overheard audio sounds ");
            sb_append_cstr(sb, fr_similarity_text(similarity, enum_name));
            sb_append_cstr(sb, ": \"");
            if (word_count > 0) {
                i32 i;
                for (i = 0; i < word_count; ++i) {
                    if (i > 0) sb_append_char(sb, ' ');
                    sb_append(sb, words[i]);
                }
            } else {
                sb_append(sb, text);
            }
            sb_append_cstr(sb, "\"\n\n");
            arena_reset_to(&im->strings, mark);
        }
    }
}

/* The base64 as it sits in the document. Nothing is decoded here and
 * nothing is copied: a photograph is a view until it has somewhere to
 * go. */
static void fr_read_base64_array(Json_Parser *p, String *out, i32 limit,
                                 i32 *out_count)
{
    json_array_begin(p);
    while (json_array_next(p)) {
        String encoded = json_string_raw(p);
        if (*out_count < limit) out[*out_count] = encoded;
        *out_count += 1;
    }
    if (*out_count > limit) *out_count = limit;
}

static void fr_read_chat_event(Fr_Import *im, Json_Parser *p,
                               Fr_Imported_Event *ev)
{
    String key;
    String text_data = str_nil;
    String voice_text = str_nil;
    String notice     = str_nil;
    String summary    = str_nil;
    String overheard  = str_nil;
    b32    is_user    = 0;

    memset(ev, 0, sizeof(*ev));
    ev->kind = FRIEND_MESSAGE_FRIEND;
    ev->turn = FR_TURN_FRIEND;

    json_object_begin(p);
    while (json_object_next(p, &key)) {
        if (fr_field_is(key, "isUserMessage")) {
            is_user = json_bool(p);
        } else if (fr_field_is(key, "textData")) {
            text_data = json_string(p, &im->strings);
        } else if (fr_field_is(key, "voiceResponseText")) {
            voice_text = json_string(p, &im->strings);
        } else if (fr_field_is(key, "systemNoticeText")) {
            notice = json_string(p, &im->strings);
        } else if (fr_field_is(key, "overheardSummaryText")) {
            summary = json_string(p, &im->strings);
        } else if (fr_field_is(key, "dateOccured")) {
            ev->at_unix_ms = json_timestamp_ms(p);
        } else if (fr_field_is(key, "wasTranscribed")) {
            ev->was_transcribed = json_bool(p);
        } else if (fr_field_is(key, "interruptedFraction")) {
            ev->interrupted_fraction = (f32)json_f64(p);
        } else if (fr_field_is(key, "overheardDiarizedTranscription")) {
            String_Builder sb = sb_begin(&im->build);
            fr_read_transcription(im, p, &sb);
            overheard = sb_done(&sb);
        } else if (fr_field_is(key, "imageCaptions")) {
            json_array_begin(p);
            while (json_array_next(p)) {
                String caption = json_string(p, &im->strings);
                if (ev->caption_count < FR_MEMORY_IMAGES_MAX) {
                    ev->captions[ev->caption_count] = caption;
                }
                ev->caption_count += 1;
            }
            if (ev->caption_count > FR_MEMORY_IMAGES_MAX) {
                ev->caption_count = FR_MEMORY_IMAGES_MAX;
            }
        } else if (fr_field_is(key, "imageJpegs")) {
            fr_read_base64_array(p, ev->images, FR_MEMORY_IMAGES_MAX,
                                 &ev->image_count);
        } else if (fr_field_is(key, "imageThumbnailsJpeg")) {
            fr_read_base64_array(p, ev->thumbs, FR_MEMORY_IMAGES_MAX,
                                 &ev->thumb_count);
        } else {
            /* emotion, hardwareContext, voiceAudioMp3, imageRefs: either
             * the service's own bookkeeping or already inlined elsewhere
             * by the exporter. */
            json_skip(p);
        }
    }

    /* The order matters: a notice is not a message at all, an ambient
     * event is not the user speaking even though it is stored on a user
     * event, and only then does is_user decide. */
    if (notice.len > 0) {
        ev->kind = FRIEND_MESSAGE_NOTICE;
        ev->turn = FR_TURN_USER; /* notices are left out of dialogue */
        ev->text = notice;
    } else if (summary.len > 0) {
        ev->kind = FRIEND_MESSAGE_AMBIENT;
        ev->turn = FR_TURN_AMBIENT;
        ev->text = summary;
    } else if (overheard.len > 0) {
        ev->kind = FRIEND_MESSAGE_AMBIENT;
        ev->turn = FR_TURN_OVERHEARD;
        ev->text = overheard;
    } else if (is_user) {
        ev->kind = FRIEND_MESSAGE_USER;
        ev->turn = FR_TURN_USER;
        ev->text = text_data;
    } else {
        ev->kind = FRIEND_MESSAGE_FRIEND;
        ev->turn = FR_TURN_FRIEND;
        /* The tagged form is what the model saw of its own last reply. */
        ev->text = voice_text.len > 0 ? voice_text : text_data;
    }
}

/* Photos: the pixels into the blob file, the descriptor onto the record.
 * A record with both a full photograph and a thumbnail keeps both — the
 * big one for the person, the small one for the models, which is how the
 * service held them. Most of what the export carries is thumbnails
 * alone, and then there is one copy that serves for both.
 *
 * A photograph the friend has nowhere to put is described and not kept.
 * That is the whole of the failure: the count is right, the message
 * still says a photograph was sent, and the caption is still there. */
static void fr_attach_images(Fr_Import *im, const Fr_Imported_Event *ev,
                             Fr_Image_Ref *images, i32 *image_count, i32 limit)
{
    i32 count = ev->image_count > ev->thumb_count ? ev->image_count
                                                  : ev->thumb_count;
    i32 i;
    for (i = 0; i < count && *image_count < limit; ++i) {
        Fr_Image_Ref *ref   = &images[*image_count];
        String        full  = i < ev->image_count ? ev->images[i] : str_nil;
        String        thumb = i < ev->thumb_count ? ev->thumbs[i] : str_nil;

        if (str_is_empty(full)) {
            /* Only the small one came across. It is the photograph as
             * far as this friend is concerned. */
            full  = thumb;
            thumb = str_nil;
        }
        if (!fr_blob_stage_base64(im->fr, full, thumb, ref)) {
            im->report.images_without_pixels += 1;
        }
        *image_count += 1;
        im->report.images += 1;
    }
}

/* ------------------------------------------------------------------ *
 *  A memory
 * ------------------------------------------------------------------ */

static void fr_import_element(Fr_Import *im, Json_Parser *p)
{
    Friend_Context   *fr = im->fr;
    Fr_Memory_Record *m;
    u32               slot;
    String            key;
    String            id           = str_nil;
    i32               related_at   = -1;
    i32               image_count  = 0;
    b32               has_embedding = 0;

    slot = fr_memory_alloc(fr);
    if (slot == FR_SLOT_NONE) {
        im->report.memories_dropped += 1;
        json_skip(p);
        return;
    }
    m = &fr->memories[slot];

    json_object_begin(p);
    while (json_object_next(p, &key)) {
        if (fr_field_is(key, "id")) {
            id = json_string_raw(p);
        } else if (fr_field_is(key, "description")) {
            String text = json_string(p, &im->strings);
            if (text.len > FR_DESCRIPTION_MAX) im->report.truncated += 1;
            fr_set_description(m, text);
        } else if (fr_field_is(key, "dateCreated")) {
            m->created_unix_ms = json_timestamp_ms(p);
        } else if (fr_field_is(key, "embeddingInt8")) {
            i64 written = json_base64(p, (u8 *)m->embedding, FR_EMBED_DIMS);
            has_embedding = (written == FR_EMBED_DIMS);
        } else if (fr_field_is(key, "useCount")) {
            m->use_count = (u64)json_i64(p);
        } else if (fr_field_is(key, "usesThisEpoch")) {
            m->uses_this_epoch = (u64)json_i64(p);
        } else if (fr_field_is(key, "inThisTrackingEpoch")) {
            m->in_this_epoch = json_bool(p) ? 1 : 0;
        } else if (fr_field_is(key, "relatedEvents")) {
            /* After the rest of the element, so the dialogue is appended
             * to a record whose other fields are already settled. */
            related_at = p->at;
            json_skip(p);
        } else {
            json_skip(p);
        }
    }

    /* An embedding the configured model did not produce is not comparable
     * with the ones this friend will make from here on, so it has to be
     * derived again from the description — which the export does carry. */
    m->needs_embedding = (has_embedding && im->reuse_embeddings) ? 0 : 1;
    if (!has_embedding) memset(m->embedding, 0, sizeof(m->embedding));

    if (related_at >= 0) {
        Json_Parser rp = json_begin(im->document);
        rp.at          = related_at;
        json_array_begin(&rp);
        while (json_array_next(&rp)) {
            Fr_Imported_Event ev;
            i32               i;
            fr_import_record_begin(im);
            fr_read_chat_event(im, &rp, &ev);
            if (ev.kind != FRIEND_MESSAGE_NOTICE && ev.at_unix_ms > 0) {
                fr_dialog_append(m, ev.at_unix_ms, ev.turn, ev.text);
                m->dialog_end_unix_ms = ev.at_unix_ms;
            }
            /* Captions ride along as turns, in image order, so a memory's
             * photos and their captions stay paired without either an
             * offset to maintain or room set aside per image. */
            for (i = 0; i < ev.caption_count; ++i) {
                if (ev.captions[i].len > 0) {
                    fr_dialog_append(m, ev.at_unix_ms, FR_TURN_CAPTION,
                                     ev.captions[i]);
                }
            }
            fr_attach_images(im, &ev, m->images, &image_count,
                             FR_MEMORY_IMAGES_MAX);
        }
        if (rp.failed) im->report.truncated += 1;
    }
    m->image_count = (u8)image_count;
    if (m->dialog_end_unix_ms == 0) m->dialog_end_unix_ms = m->created_unix_ms;

    fr_id_put(&im->ids, id, slot);
    im->report.memories += 1;
}

/* ------------------------------------------------------------------ *
 *  A connection
 * ------------------------------------------------------------------ */

/* The service's ConnectionType, by its proto number. protojson writes
 * either the name or the number, so both are looked up here; the public
 * enum was numbered to match, so a number is the relation. */
static const char *const fr_connection_types[] = {
    "CONNECTION_TYPE_UNSPECIFIED", "CONNECTION_TYPE_CHANGED",
    "CONNECTION_TYPE_CAUSE",       "CONNECTION_TYPE_REASON",
    "CONNECTION_TYPE_HINDERED_BY", "CONNECTION_TYPE_REACT",
    "CONNECTION_TYPE_WANT",        "CONNECTION_TYPE_SAME_TOPIC",
};

FR_STATIC_ASSERT(FRIEND_RELATION_NONE == 0 && FRIEND_RELATION_CHANGED == 1 &&
                     FRIEND_RELATION_CAUSE == 2 &&
                     FRIEND_RELATION_REASON == 3 &&
                     FRIEND_RELATION_HINDERED_BY == 4 &&
                     FRIEND_RELATION_REACT == 5 && FRIEND_RELATION_WANT == 6 &&
                     FRIEND_RELATION_SAME_TOPIC == 7,
                 relation_is_the_proto_number);

static Friend_Relation fr_relation_from(String name, i64 number)
{
    i32 i;
    for (i = 1; i < fr_array_count(fr_connection_types); ++i) {
        if (number == i || str_eq(name, str_from_cstr(fr_connection_types[i]))) {
            return (Friend_Relation)i;
        }
    }
    return FRIEND_RELATION_NONE;
}

static void fr_import_connection(Fr_Import *im, Json_Parser *p)
{
    String key;
    String from_id = str_nil, to_id = str_nil, type_name = str_nil;
    i64    type_number = 0;
    u32    from, to;

    json_object_begin(p);
    while (json_object_next(p, &key)) {
        if (fr_field_is(key, "fromId")) {
            from_id = json_string_raw(p);
        } else if (fr_field_is(key, "toId")) {
            to_id = json_string_raw(p);
        } else if (fr_field_is(key, "connectionType")) {
            if (json_peek(p) == JSON_STRING) type_name = json_string_raw(p);
            else type_number = json_i64(p);
        } else {
            json_skip(p);
        }
    }

    /* Either end may have been a memory we had no room for. */
    from = fr_id_get(&im->ids, from_id);
    to   = fr_id_get(&im->ids, to_id);
    if (!fr_edge_add(im->fr, from, to,
                     (u8)fr_relation_from(type_name, type_number))) {
        im->report.connections_dropped += 1;
        return;
    }
    im->report.connections += 1;
}

/* ------------------------------------------------------------------ *
 *  The conversation
 * ------------------------------------------------------------------ */

static void fr_fill_event(Fr_Chat_Event *slot, const Fr_Imported_Event *ev,
                           u64 index, Fr_Import *im)
{
    String text = ev->text;
    i32    images = 0;

    memset(slot, 0, sizeof(*slot));
    slot->index            = index;
    slot->occurred_unix_ms = ev->at_unix_ms;
    slot->kind             = (u8)ev->kind;
    slot->was_transcribed  = ev->was_transcribed ? 1 : 0;
    slot->ambient_raw      = ev->turn == FR_TURN_OVERHEARD ? 1 : 0;
    slot->interrupted_fraction = ev->interrupted_fraction;

    if (text.len > FR_CHAT_TEXT_MAX) im->report.truncated += 1;
    text           = fr_truncate_utf8(text, FR_CHAT_TEXT_MAX);
    slot->text_len = (u16)text.len;
    if (text.len > 0) memcpy(slot->text, text.data, (size_t)text.len);

    fr_attach_images(im, ev, slot->images, &images, FR_CHAT_IMAGES_MAX);
    slot->image_count = (u8)images;

    /* What a vision model made of each photograph at the time, which is
     * what is left of a photograph once a prompt has to be built
     * without the pixels. */
    if (images > 0 && ev->caption_count > 0) {
        String captions[FR_CHAT_IMAGES_MAX];
        i32    i;
        for (i = 0; i < images; ++i) {
            captions[i] = i < ev->caption_count ? ev->captions[i] : str_nil;
        }
        fr_set_captions(slot, captions, images);
    }
}

/* The scrollback. An export of any size fits it — the limits are set so
 * that a decade of talking does — and if one does not, the oldest fall
 * off the front and are counted as dropped.
 *
 * The record is built on the stack and then packed into the stream,
 * which is eight kilobytes of frame for a few hundred bytes of message.
 * It is the one place that is worth it: the alternative is a second
 * version of fr_fill_event that writes packed records, and two ways to
 * build a message from an export is how they come to disagree. */
static void fr_push_recent(Fr_Import *im, const Fr_Imported_Event *ev,
                           u64 index)
{
    Friend_Context *fr = im->fr;
    Fr_Chat_Event   event;
    u64             before = fr->scroll.lost;

    fr_fill_event(&event, ev, index, im);
    if (!fr_scroll_push(fr, &event) || fr->scroll.lost != before) {
        im->report.messages_dropped += 1;
    }
}

static void fr_push_active(Fr_Import *im, const Fr_Imported_Event *ev,
                           u64 index)
{
    Friend_Context *fr = im->fr;
    if (fr->active_count >= fr->active_capacity) {
        fr_push_recent(im, ev, index);
        return;
    }
    fr_fill_event(&fr->active_events[fr->active_count], ev, index, im);
    fr->active_count += 1;
}

static i32 fr_count_array(String document, i32 at)
{
    Json_Parser p = json_begin(document);
    i32         count = 0;
    p.at              = at;
    json_array_begin(&p);
    while (json_array_next(&p)) {
        json_skip(&p);
        ++count;
    }
    return p.failed ? 0 : count;
}

/* The sections below are read by parsers of their own, starting where
 * the first pass noted them. Each returns why its parser failed, or
 * nothing. */

/* StoredChatEvent is a ChatEvent with the index the service gave it. */
static String fr_import_events(Fr_Import *im, i32 at, b32 into_active)
{
    Json_Parser p;
    i32         total = fr_count_array(im->document, at);
    i32         seen  = 0;
    /* Everything above the window the model sees is scrollback, and the
     * oldest of it is what falls off the end. Counting first means the
     * oldest go straight to the ring instead of being shuffled through
     * the active array one memcpy at a time. */
    i32 overflow = into_active ? total - im->fr->active_capacity : total;

    p    = json_begin(im->document);
    p.at = at;
    json_array_begin(&p);
    while (json_array_next(&p)) {
        Fr_Imported_Event ev;
        String            key;
        u64               index    = 0;
        b32               have_event = 0;

        fr_import_record_begin(im);
        memset(&ev, 0, sizeof(ev));

        json_object_begin(&p);
        while (json_object_next(&p, &key)) {
            if (fr_field_is(key, "index")) {
                index = (u64)json_i64(&p);
            } else if (fr_field_is(key, "chatEvent")) {
                fr_read_chat_event(im, &p, &ev);
                have_event = 1;
            } else {
                json_skip(&p);
            }
        }
        if (!have_event) continue;
        if (seen < overflow) fr_push_recent(im, &ev, index);
        else fr_push_active(im, &ev, index);
        ++seen;
    }
    return p.failed ? p.error : str_nil;
}

/* ------------------------------------------------------------------ *
 *  MemoryState
 * ------------------------------------------------------------------ */

static String fr_import_memory_state(Fr_Import *im, i32 at)
{
    Json_Parser p;
    String      key;
    i32         connections_at = -1;

    p    = json_begin(im->document);
    p.at = at;
    json_object_begin(&p);
    while (json_object_next(&p, &key)) {
        if (fr_field_is(key, "elements")) {
            json_array_begin(&p);
            while (json_array_next(&p)) {
                fr_import_record_begin(im);
                fr_import_element(im, &p);
            }
        } else if (fr_field_is(key, "connections")) {
            /* Named by id, so they wait until every element has a slot. */
            connections_at = p.at;
            json_skip(&p);
        } else {
            json_skip(&p); /* memoryCount, which was never written */
        }
    }

    if (p.failed) return p.error;

    if (connections_at >= 0) {
        Json_Parser cp = json_begin(im->document);
        cp.at          = connections_at;
        json_array_begin(&cp);
        while (json_array_next(&cp)) fr_import_connection(im, &cp);
        if (cp.failed) return cp.error;
    }
    return str_nil;
}

/* ------------------------------------------------------------------ *
 *  The whole thing
 * ------------------------------------------------------------------ */

static Friend_Result fr_import_setup(Fr_Import *im)
{
    Friend_Context *fr = im->fr;
    size_t          each;

    if (!fr_id_table_make(&im->ids, &fr->scratch, fr->memory_capacity)) {
        return fr_error(fr, FRIEND_ERR_FULL,
                        str_lit("not enough scratch memory to index the "
                                "export's memories"));
    }
    /* Whatever is left, split in two. A field bigger than half the
     * remaining scratch comes out truncated rather than taking the import
     * down with it. */
    each = arena_remaining(&fr->scratch) / 2;
    if (each < FR_KB(64)) {
        return fr_error(fr, FRIEND_ERR_FULL,
                        str_lit("not enough scratch memory to import"));
    }
    im->strings = arena_sub(&fr->scratch, each, 16);
    im->build   = arena_sub(&fr->scratch, each, 16);
    if (!im->strings.base || !im->build.base) {
        return fr_error(fr, FRIEND_ERR_FULL,
                        str_lit("not enough scratch memory to import"));
    }
    return FRIEND_OK;
}

Friend_Result friend_import_export(Friend_Context *fr, Friend_String json,
                                   Friend_Import_Report *out_report)
{
    Fr_Import     im;
    Json_Parser   p;
    Arena_Mark    mark;
    String        key;
    Friend_Result result;
    String        why;
    i32           memory_state_at = -1, active_at = -1, history_at = -1;

    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    if (json.len <= 0 || !json.data) {
        return fr_error(fr, FRIEND_ERR_BAD_ARGUMENT,
                        str_lit("the export is empty"));
    }

    mark = arena_mark(&fr->scratch);
    memset(&im, 0, sizeof(im));
    im.fr       = fr;
    im.document = json;
    /* Byte-for-byte reuse only holds while the vectors are comparable,
     * which means the same model that made them. */
    im.reuse_embeddings = str_eq(fr->models.embed, FR_EXPORT_EMBED_MODEL);

    result = fr_import_setup(&im);
    if (result != FRIEND_OK) {
        arena_reset_to(&fr->scratch, mark);
        return result;
    }

    fr_state_clear(fr);

    p = json_begin(json);
    json_object_begin(&p);
    while (json_object_next(&p, &key)) {
        if (fr_field_is(key, "friendName")) {
            fr_import_record_begin(&im);
            fr_set_name(fr, json_string(&p, &im.strings));
        } else if (fr_field_is(key, "interiority")) {
            fr_import_record_begin(&im);
            fr_set_interiority(fr, json_string(&p, &im.strings));
        } else if (fr_field_is(key, "totalChatEventCount")) {
            fr->total_event_count = (u64)json_i64(&p);
        } else if (fr_field_is(key, "memoryTrackingResponseCount")) {
            fr->replies_total = (u64)json_i64(&p);
        } else if (fr_field_is(key, "totalSamplesThisEpoch")) {
            fr->epoch_samples = (u64)json_i64(&p);
        } else if (fr_field_is(key, "epochActive")) {
            fr->epoch_active = json_bool(&p);
        } else if (fr_field_is(key, "dateCreated")) {
            fr->created_unix_ms = json_timestamp_ms(&p);
        } else if (fr_field_is(key, "memoryState")) {
            memory_state_at = p.at;
            json_skip(&p);
        } else if (fr_field_is(key, "activeChatEvents")) {
            active_at = p.at;
            json_skip(&p);
        } else if (fr_field_is(key, "messageHistory")) {
            history_at = p.at;
            json_skip(&p);
        } else {
            /* userId, personalityVersion, overheardQueue and the fields
             * the exporter already stripped. */
            json_skip(&p);
        }
    }

    why = p.failed ? p.error : str_nil;
    if (str_is_empty(why) && memory_state_at >= 0) {
        why = fr_import_memory_state(&im, memory_state_at);
    }
    /* History first: it is older than the active chat by definition, and
     * the ring has to end up in order. */
    if (str_is_empty(why) && history_at >= 0) {
        why = fr_import_events(&im, history_at, 0);
    }
    if (str_is_empty(why) && active_at >= 0) {
        why = fr_import_events(&im, active_at, 1);
    }
    if (!str_is_empty(why)) {
        arena_reset_to(&fr->scratch, mark);
        fr_state_clear(fr);
        return fr_error(fr, FRIEND_ERR_BAD_DATA, why);
    }

    /* Counted at the end, so it means what the friend holds rather than
     * what passed through the ring on the way. */
    im.report.messages = fr->active_count + fr->scroll.count;
    if (fr->total_event_count == 0) {
        fr->total_event_count = (u64)im.report.messages;
    }
    /* The memories came across with the conversation, so the friend
     * already remembers what it was going to remember from it. Without
     * this the first consolidation would try to catch up on a decade of
     * history the service had already folded. */
    fr->folded_through = fr->total_event_count;
    fr_mark_dirty(fr);

    if (out_report) *out_report = im.report;
    arena_reset_to(&fr->scratch, mark);
    return FRIEND_OK;
}
