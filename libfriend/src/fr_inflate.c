/* fr_inflate.c — the download zip, a chunk at a time.
 *
 * The export is a zip holding one deflated entry. Every inflate library I
 * could vendor wants to write the whole output somewhere: sinfl's
 * sinflate() takes a destination buffer and a capacity, and the capacity
 * here is "however big that user's friend turned out to be" — a gigabyte
 * at the service's own cap. That is precisely the allocation this library
 * does not have, so this is DEFLATE written to suspend instead: it holds a
 * 32 KiB window (the format's own back-reference limit, so not a buffer we
 * chose) and hands bytes to the caller as they come.
 *
 * Huffman decoding is Mark Adler's puff method — canonical codes decoded
 * bit at a time from counts and symbols, no lookup tables to build. It is
 * slower than a table-driven decoder and completely clear, which is the
 * right trade for something that runs once per import.
 *
 * All the state is in the struct, so a push that runs out of input or out
 * of room returns and is called again. The only ordering rule is the one
 * the format imposes: a match may reach 32 KiB back, so the window has to
 * keep that much, and we only decode a symbol when there is room for the
 * longest one (258 bytes) without overwriting what the caller has not
 * taken yet.
 */

#include "../../base/fr_base.h"

#define UZ_WINDOW_BITS 15
#define UZ_WINDOW_SIZE ((u32)1 << UZ_WINDOW_BITS)
#define UZ_WINDOW_MASK (UZ_WINDOW_SIZE - 1)
#define UZ_MAX_MATCH 258
#define UZ_MAGIC 0x554E5A31u /* "UNZ1" */

/* Local file header: signature, then 26 bytes of fields. */
#define UZ_LOCAL_HEADER_BYTES 30
#define UZ_LOCAL_SIG 0x04034b50u
#define UZ_DESCRIPTOR_SIG 0x08074b50u

#define UZ_FLAG_ENCRYPTED 0x0001u
#define UZ_FLAG_DESCRIPTOR 0x0008u

#define UZ_METHOD_STORE 0
#define UZ_METHOD_DEFLATE 8

#define UZ_NAME_MAX 256

/* How many zero bits we are willing to invent once the caller says the
 * file is over. A valid stream never needs more than one symbol's worth:
 * this exists so a truncated file fails instead of looping. */
#define UZ_ZERO_BUDGET 64

typedef enum Uz_State {
    UZ_HEADER = 0,
    UZ_NAME,
    UZ_EXTRA,
    UZ_STORED_ENTRY,
    UZ_BLOCK,
    UZ_RAW_LEN,
    UZ_RAW_DATA,
    UZ_DYN_HEADER,
    UZ_DYN_CODELENS,
    UZ_DYN_LENS,
    UZ_SYMBOL,
    UZ_TRAILER,
    UZ_FLUSHING,
    UZ_FINISHED,
    UZ_FAILED
} Uz_State;

/* Canonical Huffman: count[len] codes of each length, symbol[] in code
 * order. 288 is the literal/length alphabet; the distance alphabet is 30
 * and shares the shape. */
typedef struct Uz_Huffman {
    u16 count[16];
    u16 symbol[288];
} Uz_Huffman;

struct Friend_Unzip {
    u32      magic;
    Uz_State state;
    char     error[128];
    i32      error_len;

    /* Bit reader. Bits arrive least-significant first, which is DEFLATE's
     * convention and the opposite of the zip header's byte fields. */
    u64 bitbuf;
    i32 bitcnt;
    b32 input_ended;
    i32 zeros_shifted;

    /* Zip entry. */
    u8   header[UZ_LOCAL_HEADER_BYTES];
    i32  header_filled;
    u32  flags;
    u32  method;
    u32  header_crc;
    u64  compressed_size;
    i32  name_len;
    i32  extra_len;
    i32  skip_remaining;
    char name[UZ_NAME_MAX];
    i32  name_len_stored;
    u64  stored_remaining;
    u8   trailer[8];
    i32  trailer_filled;

    /* Deflate. */
    b32 final_block;
    i32 raw_remaining;
    i32 hlit, hdist, hclen;
    i32 lens_filled;
    u16 lens[320];
    Uz_Huffman lencode;
    Uz_Huffman distcode;

    /* Output window. flush_pos trails wpos by whatever the caller has not
     * taken yet; the gap is what stops us overwriting it. */
    u64 wpos;
    u64 flush_pos;
    u64 total_out;

    u32 crc;
    u32 crc_table[256];

    u8 window[UZ_WINDOW_SIZE];
};

/* ------------------------------------------------------------------ *
 *  Scaffolding
 * ------------------------------------------------------------------ */

static Friend_Result uz_fail(Friend_Unzip *z, const char *message)
{
    i32 n = (i32)strlen(message);
    if (n > (i32)sizeof(z->error) - 1) n = (i32)sizeof(z->error) - 1;
    memcpy(z->error, message, (size_t)n);
    z->error_len = n;
    z->state     = UZ_FAILED;
    return FRIEND_ERR_BAD_DATA;
}

static void uz_crc_table(Friend_Unzip *z)
{
    u32 i, j, c;
    for (i = 0; i < 256; ++i) {
        c = i;
        for (j = 0; j < 8; ++j) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        z->crc_table[i] = c;
    }
}

/* 1 if n bits are now in the buffer, 0 if the caller must bring more
 * input, -1 if the file ended mid-symbol. */
static i32 uz_ensure(Friend_Unzip *z, i32 n, Friend_Unzip_Step *s)
{
    while (z->bitcnt < n) {
        if (s->in_used < s->in_size) {
            u8 byte = ((const u8 *)s->in)[s->in_used++];
            z->bitbuf |= (u64)byte << z->bitcnt;
            z->bitcnt += 8;
        } else if (z->input_ended) {
            if (z->zeros_shifted >= UZ_ZERO_BUDGET) return -1;
            z->zeros_shifted += 8;
            z->bitcnt += 8; /* the bits are already zero */
        } else {
            return 0;
        }
    }
    return 1;
}

static u32 uz_take(Friend_Unzip *z, i32 n)
{
    u32 value;
    if (n <= 0) return 0;
    value = (u32)(z->bitbuf & (((u64)1 << n) - 1));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return value;
}

static void uz_flush(Friend_Unzip *z, Friend_Unzip_Step *s)
{
    while (z->flush_pos < z->wpos && s->out_written < s->out_size) {
        size_t offset    = (size_t)(z->flush_pos & UZ_WINDOW_MASK);
        size_t available = (size_t)(z->wpos - z->flush_pos);
        size_t room      = s->out_size - s->out_written;
        size_t n         = available < room ? available : room;
        if (n > UZ_WINDOW_SIZE - offset) n = UZ_WINDOW_SIZE - offset;
        memcpy((u8 *)s->out + s->out_written, z->window + offset, n);
        s->out_written += n;
        z->flush_pos += n;
    }
}

/* Room in the window for `need` more bytes without losing any the caller
 * has not taken. Flushes to make room, so a small out buffer costs calls
 * rather than correctness. */
static b32 uz_room(Friend_Unzip *z, Friend_Unzip_Step *s, u32 need)
{
    if (z->wpos - z->flush_pos + need <= UZ_WINDOW_SIZE) return 1;
    uz_flush(z, s);
    return z->wpos - z->flush_pos + need <= UZ_WINDOW_SIZE;
}

static void uz_put(Friend_Unzip *z, u8 byte)
{
    z->window[z->wpos & UZ_WINDOW_MASK] = byte;
    z->wpos++;
    z->total_out++;
    z->crc = z->crc_table[(z->crc ^ byte) & 0xFFu] ^ (z->crc >> 8);
}

/* ------------------------------------------------------------------ *
 *  Huffman
 * ------------------------------------------------------------------ */

/* Builds count/symbol from a list of code lengths. Returns 0 for a
 * complete code, >0 for an incomplete one (legal for a distance tree with
 * a single code) and <0 for over-subscribed, which is corruption. */
static i32 uz_construct(Uz_Huffman *h, const u16 *lengths, i32 count)
{
    i32 symbol, len, left;
    u16 offsets[16];

    for (len = 0; len < 16; ++len) h->count[len] = 0;
    for (symbol = 0; symbol < count; ++symbol) h->count[lengths[symbol]]++;
    if (h->count[0] == count) return 0; /* no codes at all */

    left = 1;
    for (len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= (i32)h->count[len];
        if (left < 0) return left;
    }

    offsets[0] = 0;
    offsets[1] = 0;
    for (len = 1; len < 15; ++len) {
        offsets[len + 1] = (u16)(offsets[len] + h->count[len]);
    }
    for (symbol = 0; symbol < count; ++symbol) {
        if (lengths[symbol]) h->symbol[offsets[lengths[symbol]]++] = (u16)symbol;
    }
    return left;
}

/* Walks the code tree one bit at a time. Needs at most 15 bits, which the
 * caller has already made sure of, so this cannot suspend. */
static i32 uz_decode(Friend_Unzip *z, const Uz_Huffman *h)
{
    i32 code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; ++len) {
        i32 count;
        code |= (i32)uz_take(z, 1);
        count = (i32)h->count[len];
        if (code - count < first) return (i32)h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void uz_fixed_tables(Friend_Unzip *z)
{
    i32 i;
    for (i = 0; i < 144; ++i) z->lens[i] = 8;
    for (; i < 256; ++i) z->lens[i] = 9;
    for (; i < 280; ++i) z->lens[i] = 7;
    for (; i < 288; ++i) z->lens[i] = 8;
    uz_construct(&z->lencode, z->lens, 288);
    for (i = 0; i < 30; ++i) z->lens[i] = 5;
    uz_construct(&z->distcode, z->lens, 30);
}

/* The format's tables, from RFC 1951 section 3.2.5. */
static const u16 uz_length_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u16 uz_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const u16 uz_dist_base[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577
};
static const u16 uz_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* The code-length alphabet is sent in this order, commonest first. */
static const u8 uz_codelen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ------------------------------------------------------------------ *
 *  The API
 * ------------------------------------------------------------------ */

size_t friend_unzip_size(void)
{
    return sizeof(Friend_Unzip) + FR_ALIGNOF(struct Friend_Unzip) - 1;
}

Friend_Result friend_unzip_begin(void *memory, size_t size, Friend_Unzip **out)
{
    Friend_Unzip *z;
    size_t        align = FR_ALIGNOF(struct Friend_Unzip);
    size_t        shift;

    if (!out) return FRIEND_ERR_BAD_ARGUMENT;
    *out = 0;
    if (!memory) return FRIEND_ERR_BAD_ARGUMENT;
    if (size < friend_unzip_size()) return FRIEND_ERR_TOO_SMALL;

    shift = fr_align_up((size_t)(uintptr_t)memory, align) - (size_t)(uintptr_t)memory;
    z     = (Friend_Unzip *)((u8 *)memory + shift);

    memset(z, 0, sizeof(*z));
    z->magic = UZ_MAGIC;
    z->state = UZ_HEADER;
    z->crc   = 0xFFFFFFFFu;
    uz_crc_table(z);

    *out = z;
    return FRIEND_OK;
}

Friend_String friend_unzip_entry_name(Friend_Unzip *z)
{
    if (!z || z->magic != UZ_MAGIC) return str_nil;
    return str_make(z->name, z->name_len_stored);
}

Friend_String friend_unzip_error(Friend_Unzip *z)
{
    if (!z || z->magic != UZ_MAGIC) return str_nil;
    return str_make(z->error, z->error_len);
}

/* Reads one byte of a header or of a stored block. Headers are byte
 * aligned by construction, but going through the bit buffer means a
 * trailer read still sees bytes the symbol decoder pulled in early. */
static i32 uz_byte(Friend_Unzip *z, Friend_Unzip_Step *s, u8 *out)
{
    i32 ready = uz_ensure(z, 8, s);
    if (ready != 1) return ready;
    *out = (u8)uz_take(z, 8);
    return 1;
}

#define UZ_NEED_BITS(n)                                                       \
    do {                                                                      \
        i32 ready_ = uz_ensure(z, (n), s);                                    \
        if (ready_ == 0) return FRIEND_OK;                                    \
        if (ready_ < 0) return uz_fail(z, "the file ends mid-stream");         \
    } while (0)

#define UZ_NEED_BYTE(var)                                                     \
    do {                                                                      \
        i32 ready_ = uz_byte(z, s, &(var));                                   \
        if (ready_ == 0) return FRIEND_OK;                                     \
        if (ready_ < 0) return uz_fail(z, "the file ends mid-stream");         \
    } while (0)

Friend_Result friend_unzip_push(Friend_Unzip *z, Friend_Unzip_Step *s)
{
    if (!z || z->magic != UZ_MAGIC || !s) return FRIEND_ERR_BAD_ARGUMENT;

    s->in_used     = 0;
    s->out_written = 0;
    s->done        = 0;
    if (s->in_size > 0 && !s->in) return FRIEND_ERR_BAD_ARGUMENT;
    if (s->out_size > 0 && !s->out) return FRIEND_ERR_BAD_ARGUMENT;
    if (s->in_last) z->input_ended = 1;

    if (z->state == UZ_FAILED) return FRIEND_ERR_BAD_DATA;

    for (;;) {
        switch (z->state) {

        case UZ_HEADER: {
            while (z->header_filled < UZ_LOCAL_HEADER_BYTES) {
                u8 byte;
                UZ_NEED_BYTE(byte);
                z->header[z->header_filled++] = byte;
            }
            if (fr_le_get32(z->header) != UZ_LOCAL_SIG) {
                return uz_fail(z, "not a zip file: no local header where one "
                                  "should be");
            }
            z->flags           = fr_le_get16(z->header + 6);
            z->method          = fr_le_get16(z->header + 8);
            z->header_crc      = fr_le_get32(z->header + 14);
            z->compressed_size = fr_le_get32(z->header + 18);
            z->name_len        = (i32)fr_le_get16(z->header + 26);
            z->extra_len       = (i32)fr_le_get16(z->header + 28);

            if (z->flags & UZ_FLAG_ENCRYPTED) {
                return uz_fail(z, "the entry is password protected");
            }
            if (z->method != UZ_METHOD_DEFLATE && z->method != UZ_METHOD_STORE) {
                return uz_fail(z, "the entry uses a compression method this "
                                  "does not know");
            }
            z->skip_remaining = z->name_len;
            z->state          = UZ_NAME;
        } break;

        case UZ_NAME: {
            while (z->skip_remaining > 0) {
                u8 byte;
                UZ_NEED_BYTE(byte);
                z->skip_remaining--;
                if (z->name_len_stored < UZ_NAME_MAX) {
                    z->name[z->name_len_stored++] = (char)byte;
                }
            }
            z->skip_remaining = z->extra_len;
            z->state          = UZ_EXTRA;
        } break;

        case UZ_EXTRA: {
            while (z->skip_remaining > 0) {
                u8 byte;
                UZ_NEED_BYTE(byte);
                z->skip_remaining--;
            }
            if (z->method == UZ_METHOD_STORE) {
                /* Without a size there is nothing to say where a stored
                 * entry ends, and the central directory is at the far end
                 * of the file. The export is always deflated. */
                if (z->flags & UZ_FLAG_DESCRIPTOR) {
                    return uz_fail(z, "the entry is stored without a length");
                }
                z->stored_remaining = z->compressed_size;
                z->state            = UZ_STORED_ENTRY;
            } else {
                z->state = UZ_BLOCK;
            }
        } break;

        case UZ_STORED_ENTRY: {
            while (z->stored_remaining > 0) {
                u8 byte;
                if (!uz_room(z, s, 1)) return FRIEND_OK;
                UZ_NEED_BYTE(byte);
                uz_put(z, byte);
                z->stored_remaining--;
            }
            z->state = UZ_TRAILER;
        } break;

        case UZ_BLOCK: {
            UZ_NEED_BITS(3);
            z->final_block = (b32)uz_take(z, 1);
            switch (uz_take(z, 2)) {
            case 0: z->state = UZ_RAW_LEN; break;
            case 1:
                uz_fixed_tables(z);
                z->state = UZ_SYMBOL;
                break;
            case 2: z->state = UZ_DYN_HEADER; break;
            default: return uz_fail(z, "reserved block type");
            }
        } break;

        case UZ_RAW_LEN: {
            u32 len, nlen;
            /* An uncompressed block restarts on a byte boundary. */
            uz_take(z, z->bitcnt & 7);
            UZ_NEED_BITS(32);
            len  = uz_take(z, 16);
            nlen = uz_take(z, 16);
            if (((len ^ 0xFFFFu) & 0xFFFFu) != nlen) {
                return uz_fail(z, "uncompressed block length does not match "
                                  "its complement");
            }
            z->raw_remaining = (i32)len;
            z->state         = UZ_RAW_DATA;
        } break;

        case UZ_RAW_DATA: {
            while (z->raw_remaining > 0) {
                u8 byte;
                if (!uz_room(z, s, 1)) return FRIEND_OK;
                UZ_NEED_BYTE(byte);
                uz_put(z, byte);
                z->raw_remaining--;
            }
            z->state = z->final_block ? UZ_TRAILER : UZ_BLOCK;
        } break;

        case UZ_DYN_HEADER: {
            UZ_NEED_BITS(14);
            z->hlit        = (i32)uz_take(z, 5) + 257;
            z->hdist       = (i32)uz_take(z, 5) + 1;
            z->hclen       = (i32)uz_take(z, 4) + 4;
            z->lens_filled = 0;
            if (z->hlit > 286 || z->hdist > 30) {
                return uz_fail(z, "too many codes in a dynamic block");
            }
            z->state = UZ_DYN_CODELENS;
        } break;

        case UZ_DYN_CODELENS: {
            /* The lengths of the code-length codes, in their own order. */
            while (z->lens_filled < z->hclen) {
                UZ_NEED_BITS(3);
                z->lens[uz_codelen_order[z->lens_filled]] = (u16)uz_take(z, 3);
                z->lens_filled++;
            }
            for (; z->lens_filled < 19; ++z->lens_filled) {
                z->lens[uz_codelen_order[z->lens_filled]] = 0;
            }
            if (uz_construct(&z->lencode, z->lens, 19) != 0) {
                return uz_fail(z, "the code-length code is not a valid "
                                  "Huffman code");
            }
            z->lens_filled = 0;
            z->state       = UZ_DYN_LENS;
        } break;

        case UZ_DYN_LENS: {
            i32 total = z->hlit + z->hdist;
            while (z->lens_filled < total) {
                i32 symbol, repeat, value = 0;
                /* 15 for the code plus at most 7 extra bits: one step. */
                UZ_NEED_BITS(22);
                symbol = uz_decode(z, &z->lencode);
                if (symbol < 0) return uz_fail(z, "bad code length");
                if (symbol < 16) {
                    z->lens[z->lens_filled++] = (u16)symbol;
                    continue;
                }
                if (symbol == 16) {
                    if (z->lens_filled == 0) {
                        return uz_fail(z, "a repeat with nothing to repeat");
                    }
                    value  = z->lens[z->lens_filled - 1];
                    repeat = 3 + (i32)uz_take(z, 2);
                } else if (symbol == 17) {
                    repeat = 3 + (i32)uz_take(z, 3);
                } else {
                    repeat = 11 + (i32)uz_take(z, 7);
                }
                if (z->lens_filled + repeat > total) {
                    return uz_fail(z, "code lengths overrun their count");
                }
                while (repeat-- > 0) z->lens[z->lens_filled++] = (u16)value;
            }
            if (z->lens[256] == 0) {
                return uz_fail(z, "the block has no end-of-block code");
            }
            if (uz_construct(&z->lencode, z->lens, z->hlit) != 0) {
                return uz_fail(z, "the literal code is not a valid Huffman "
                                  "code");
            }
            /* An incomplete distance code is legal when there is only one
             * of them, so only over-subscription is fatal here. */
            if (uz_construct(&z->distcode, z->lens + z->hlit, z->hdist) < 0) {
                return uz_fail(z, "the distance code is over-subscribed");
            }
            z->state = UZ_SYMBOL;
        } break;

        case UZ_SYMBOL: {
            for (;;) {
                i32 symbol, length, dist_symbol;
                u32 distance;

                if (!uz_room(z, s, UZ_MAX_MATCH)) return FRIEND_OK;
                /* A whole match is 15 + 5 + 15 + 13 bits at worst, so one
                 * fill covers it and nothing below can suspend. */
                UZ_NEED_BITS(48);

                symbol = uz_decode(z, &z->lencode);
                if (symbol < 0) return uz_fail(z, "bad literal or length code");
                if (symbol < 256) {
                    uz_put(z, (u8)symbol);
                    continue;
                }
                if (symbol == 256) break; /* end of block */

                symbol -= 257;
                if (symbol >= 29) return uz_fail(z, "invalid length code");
                length = (i32)uz_length_base[symbol] +
                         (i32)uz_take(z, (i32)uz_length_extra[symbol]);

                dist_symbol = uz_decode(z, &z->distcode);
                if (dist_symbol < 0 || dist_symbol >= 30) {
                    return uz_fail(z, "bad distance code");
                }
                distance = (u32)uz_dist_base[dist_symbol] +
                           uz_take(z, (i32)uz_dist_extra[dist_symbol]);
                if ((u64)distance > z->total_out) {
                    return uz_fail(z, "a match reaches back before the start "
                                      "of the data");
                }
                while (length-- > 0) {
                    uz_put(z, z->window[(z->wpos - distance) & UZ_WINDOW_MASK]);
                }
            }
            z->state = z->final_block ? UZ_TRAILER : UZ_BLOCK;
        } break;

        case UZ_TRAILER: {
            /* The stored CRC is either in the local header or, when the
             * writer did not know it in advance, in a descriptor right
             * after the data. Go's zip writer always does the latter. */
            u32 expected = z->header_crc;
            u32 computed = z->crc ^ 0xFFFFFFFFu;

            if (z->flags & UZ_FLAG_DESCRIPTOR) {
                /* Read into the struct, not a local: running out of input
                 * here has to be resumable like everywhere else. */
                uz_take(z, z->bitcnt & 7);
                while (z->trailer_filled < 4) {
                    u8 byte;
                    UZ_NEED_BYTE(byte);
                    z->trailer[z->trailer_filled++] = byte;
                }
                expected = fr_le_get32(z->trailer);
                if (expected == UZ_DESCRIPTOR_SIG) {
                    /* The signature is optional; when present the CRC is
                     * the next word. */
                    while (z->trailer_filled < 8) {
                        u8 byte;
                        UZ_NEED_BYTE(byte);
                        z->trailer[z->trailer_filled++] = byte;
                    }
                    expected = fr_le_get32(z->trailer + 4);
                }
            }
            if (expected != computed) {
                return uz_fail(z, "the file is damaged: its checksum does not "
                                  "match its contents");
            }
            z->state = UZ_FLUSHING;
        } break;

        case UZ_FLUSHING: {
            uz_flush(z, s);
            if (z->flush_pos < z->wpos) return FRIEND_OK;
            z->state = UZ_FINISHED;
        } break;

        case UZ_FINISHED: {
            s->done = 1;
            return FRIEND_OK;
        }

        case UZ_FAILED:
            return FRIEND_ERR_BAD_DATA;
        }

        /* Keep the caller's buffer as full as we can on the way out, so a
         * caller that drains after every push never stalls. */
        uz_flush(z, s);
    }
}
