/* fr_net.c — sockets, TLS, and just enough HTTP.
 *
 * One connection is a small state machine: connect, handshake, write the
 * request, read the status line and headers, then stream the body into
 * the buffer the friend provided. Several of them run at once over one
 * poll(), which is the whole concurrency story — no threads, no
 * callbacks, nothing to synchronise.
 *
 * The HTTP here is deliberately partial. It speaks what these two APIs
 * answer with and nothing else: HTTP/1.1, identity or chunked bodies, no
 * compression (we do not ask for any), no redirects (neither provider
 * issues them), and no continuation lines. Anything else is reported as
 * a failure rather than guessed at.
 *
 * One blocking call survives: name resolution. Everything after it is
 * non-blocking. Every request asks for Connection: close, so every
 * request resolves again; making that asynchronous means either a thread
 * or a resolver of our own.
 *
 * Websockets ride the same machinery. The upgrade is an ordinary GET
 * whose answer is a 101, after which the connection stops being HTTP
 * and starts being frames: masked on the way out because the protocol
 * says a client must, reassembled on the way in into the buffer the
 * friend handed over with the open request. That buffer is why the open
 * request stays outstanding for the life of the socket — it is where
 * every message lands.
 */

#include "fr_net.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha1.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define FR_NET_HOST_MAX 256
#define FR_NET_HEAD_MAX 8192
#define FR_NET_CHUNK_MAX 16384
#define FR_NET_ERROR_MAX 256

typedef enum Fr_Conn_State {
    FR_CONN_FREE = 0,
    FR_CONN_HANDSHAKE,
    FR_CONN_SENDING,
    FR_CONN_HEADERS,
    FR_CONN_BODY,
    FR_CONN_WS_UPGRADE, /* the 101 has been asked for, not yet answered */
    FR_CONN_WS          /* frames, both ways, until somebody closes     */
} Fr_Conn_State;

/* How many frames can be waiting to go out on one socket. Audio is sent
 * in tenths of a second and everything else is occasional, so this is
 * four times what a busy session uses; past it the transport says it is
 * full and the request waits in the queue with the others. */
#define FR_WS_SENDS 8

/* A control frame's payload cannot exceed 125 bytes, which is the
 * protocol's rule, not ours. */
#define FR_WS_CONTROL_MAX 128

typedef struct Fr_Ws_Send {
    uint64_t      id; /* the friend's request; 0 for a pong or a close */
    Friend_String body;
    int32_t       sent;
    uint8_t       head[14];
    int32_t       head_len;
    int32_t       head_sent;
    uint8_t       mask[4];
} Fr_Ws_Send;

typedef struct Fr_Conn {
    Fr_Conn_State state;
    uint64_t      id;
    int32_t       abandoned;

    mbedtls_net_context  net;
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   conf;
    int32_t              tls_live;
    int32_t              plain; /* http:// — only ever a local server */

    char host[FR_NET_HOST_MAX];
    char port[8];

    /* The request, built once into the connection's own buffer. The
     * body is not copied: it points into the friend's io arena, which
     * is promised to stay put until the request is completed. */
    char          *head;
    int32_t        head_len;
    int32_t        head_sent;
    Friend_String  body;
    int32_t        body_sent;

    /* The response. */
    char    status_head[FR_NET_HEAD_MAX];
    int32_t status_head_len;
    int32_t status;
    int32_t chunked;
    int64_t content_length; /* -1 when not stated */
    int64_t body_received;

    /* Chunked framing. */
    int64_t chunk_left;
    int32_t chunk_seen_size;
    int32_t chunk_done;

    uint8_t *recv;
    uint32_t recv_capacity;

    /* ------------------------------------------------------------- *
     *  Websocket
     *
     *  Everything below is dead weight on an ordinary request and the
     *  whole state of a live session on an upgraded one.
     * ------------------------------------------------------------- */
    int32_t    ws;        /* the upgrade was accepted */
    int32_t    ws_closing;/* a close frame has been queued */
    char       ws_accept[64]; /* what the server has to answer with */

    Fr_Ws_Send sends[FR_WS_SENDS];
    int32_t    send_count;

    /* The frame being read. A header is between two and fourteen bytes
     * and arrives in whatever pieces the network chose. */
    uint8_t frame[14];
    int32_t frame_len;
    int32_t frame_need;
    int64_t frame_left;    /* payload still to come */
    int32_t frame_opcode;
    int32_t frame_fin;
    int32_t frame_control;

    /* A control frame can arrive in the middle of a fragmented message,
     * so it cannot share the buffer the message is being assembled in. */
    uint8_t ctl[FR_WS_CONTROL_MAX];
    int32_t ctl_len;
    uint8_t pong[FR_WS_CONTROL_MAX];

    /* The message being assembled, in the friend's own buffer. */
    uint32_t msg_len;
    int32_t  msg_binary;

    char error[FR_NET_ERROR_MAX];
} Fr_Conn;

#define FR_NET_QUEUE_PER_CONN 4

struct Fr_Net {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         roots;

    Fr_Conn *conns;
    int32_t  conn_count;

    char *head_pool;      /* one head buffer per connection */

    /* Requests the friend handed over while every connection was busy.
     * They keep their strings — the friend promises those stay put until
     * the request is completed or failed — so queueing the description
     * itself is enough, and the friend never has to be told that its
     * transport was briefly full. */
    Friend_Io_Request *queue;
    int32_t            queue_count;
    int32_t            queue_capacity;

    char error[FR_NET_ERROR_MAX];
};

/* ------------------------------------------------------------------ *
 *  Odds and ends
 * ------------------------------------------------------------------ */

static void fr_net_say(char *dest, size_t size, const char *what, int code)
{
    if (code) {
        char detail[128];
        mbedtls_strerror(code, detail, sizeof(detail));
        snprintf(dest, size, "%s: %s (-0x%04x)", what, detail,
                 (unsigned)-code);
    } else {
        snprintf(dest, size, "%s", what);
    }
}

static size_t fr_align16(size_t v) { return (v + 15u) & ~(size_t)15u; }

/* The request head needs room for the URL's path, the header block, and
 * the handful of fields we add. Generous, and fixed. */
#define FR_NET_HEAD_BUDGET 16384

size_t fr_net_size(int32_t max_connections)
{
    if (max_connections < 1) max_connections = 1;
    return fr_align16(sizeof(struct Fr_Net)) +
           fr_align16(sizeof(Fr_Conn) * (size_t)max_connections) +
           fr_align16(FR_NET_HEAD_BUDGET * (size_t)max_connections) +
           fr_align16(sizeof(Friend_Io_Request) *
                      (size_t)(max_connections * FR_NET_QUEUE_PER_CONN));
}

/* ------------------------------------------------------------------ *
 *  Setting up
 * ------------------------------------------------------------------ */

static const char *fr_net_ca_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt",         /* Debian, Arch, Alpine */
    "/etc/pki/tls/certs/ca-bundle.crt",           /* Fedora, RHEL         */
    "/etc/ssl/ca-bundle.pem",                     /* openSUSE             */
    "/etc/ssl/cert.pem",                          /* macOS, OpenBSD       */
    "/usr/local/etc/openssl/cert.pem",            /* Homebrew             */
    0
};

/* fr_net_begin's way out when it fails: the reason to the caller, and
 * whatever mbedTLS allocated given back. */
static Fr_Net *fr_net_refuse(Fr_Net *net, char *out_error, size_t error_size,
                             const char *why)
{
    if (out_error && error_size > 0) snprintf(out_error, error_size, "%s", why);
    fr_net_end(net);
    return 0;
}

Fr_Net *fr_net_begin(void *memory, size_t size, int32_t max_connections,
                     const char *ca_bundle_path, char *out_error,
                     size_t error_size)
{
    struct Fr_Net *net;
    uint8_t       *at = (uint8_t *)memory;
    int32_t        i;
    int            rc;

    if (out_error && error_size > 0) out_error[0] = 0;
    if (!memory || max_connections < 1 ||
        size < fr_net_size(max_connections)) {
        char why[FR_NET_ERROR_MAX];
        snprintf(why, sizeof(why),
                 "the transport needs %lu bytes for %d connections and was "
                 "given %lu",
                 (unsigned long)fr_net_size(max_connections),
                 (int)max_connections, memory ? (unsigned long)size : 0ul);
        return fr_net_refuse(0, out_error, error_size, why);
    }

    net = (struct Fr_Net *)at;
    memset(net, 0, sizeof(*net));
    at += fr_align16(sizeof(*net));

    net->conns      = (Fr_Conn *)at;
    net->conn_count = max_connections;
    memset(net->conns, 0, sizeof(Fr_Conn) * (size_t)max_connections);
    at += fr_align16(sizeof(Fr_Conn) * (size_t)max_connections);

    net->head_pool = (char *)at;
    for (i = 0; i < max_connections; ++i) {
        net->conns[i].head = net->head_pool + (size_t)i * FR_NET_HEAD_BUDGET;
        mbedtls_net_init(&net->conns[i].net);
    }
    at += fr_align16(FR_NET_HEAD_BUDGET * (size_t)max_connections);

    net->queue          = (Friend_Io_Request *)at;
    net->queue_capacity = max_connections * FR_NET_QUEUE_PER_CONN;
    net->queue_count    = 0;

    mbedtls_entropy_init(&net->entropy);
    mbedtls_ctr_drbg_init(&net->drbg);
    mbedtls_x509_crt_init(&net->roots);

    rc = mbedtls_ctr_drbg_seed(&net->drbg, mbedtls_entropy_func, &net->entropy,
                               (const unsigned char *)"friend-local", 12);
    if (rc != 0) {
        fr_net_say(net->error, sizeof(net->error), "could not seed the "
                   "random number generator", rc);
        return fr_net_refuse(net, out_error, error_size, net->error);
    }

    if (ca_bundle_path && *ca_bundle_path) {
        rc = mbedtls_x509_crt_parse_file(&net->roots, ca_bundle_path);
        if (rc < 0) {
            fr_net_say(net->error, sizeof(net->error),
                       "could not read the CA bundle you named", rc);
            return fr_net_refuse(net, out_error, error_size, net->error);
        }
    } else {
        int32_t found = 0;
        for (i = 0; fr_net_ca_paths[i]; ++i) {
            if (mbedtls_x509_crt_parse_file(&net->roots, fr_net_ca_paths[i]) >=
                0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return fr_net_refuse(net, out_error, error_size,
                                 "no CA certificate bundle found: pass one, "
                                 "because without roots there is nothing to "
                                 "verify a server against");
        }
    }
    return net;
}

void fr_net_end(Fr_Net *net)
{
    int32_t i;
    if (!net) return;
    for (i = 0; i < net->conn_count; ++i) {
        Fr_Conn *c = &net->conns[i];
        if (c->tls_live) {
            mbedtls_ssl_free(&c->ssl);
            mbedtls_ssl_config_free(&c->conf);
            c->tls_live = 0;
        }
        mbedtls_net_free(&c->net);
        c->state = FR_CONN_FREE;
    }
    mbedtls_x509_crt_free(&net->roots);
    mbedtls_ctr_drbg_free(&net->drbg);
    mbedtls_entropy_free(&net->entropy);
}

int32_t fr_net_active(Fr_Net *net)
{
    int32_t i, n = 0;
    if (!net) return 0;
    for (i = 0; i < net->conn_count; ++i) {
        if (net->conns[i].state != FR_CONN_FREE) ++n;
    }
    return n;
}

static void fr_conn_close(Fr_Conn *c)
{
    if (c->tls_live) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        c->tls_live = 0;
    }
    mbedtls_net_free(&c->net);
    mbedtls_net_init(&c->net);
    c->state = FR_CONN_FREE;
}

/* ------------------------------------------------------------------ *
 *  Submitting
 * ------------------------------------------------------------------ */

static int32_t fr_split_url(const Friend_String url, Fr_Conn *c,
                            const char **out_path, int32_t *out_path_len)
{
    int32_t at = 0;
    int32_t host_start, host_end;

    c->plain = 0;
    if (url.len > 8 && memcmp(url.data, "https://", 8) == 0) {
        at = 8;
    } else if (url.len > 6 && memcmp(url.data, "wss://", 6) == 0) {
        /* The same connection with a different word in front of it. */
        at = 6;
    } else if (url.len > 7 && memcmp(url.data, "http://", 7) == 0) {
        at       = 7;
        c->plain = 1;
    } else if (url.len > 5 && memcmp(url.data, "ws://", 5) == 0) {
        at       = 5;
        c->plain = 1;
    } else {
        return 0;
    }

    host_start = at;
    while (at < url.len && url.data[at] != '/' && url.data[at] != '?') ++at;
    host_end = at;
    if (host_end == host_start) return 0;

    /* A port, if there is one. No IPv6 literals: a provider's endpoint
     * is a name, and a local server is localhost. */
    {
        int32_t colon = -1;
        int32_t i;
        for (i = host_start; i < host_end; ++i) {
            if (url.data[i] == ':') colon = i;
        }
        if (colon >= 0) {
            int32_t plen = host_end - colon - 1;
            if (plen <= 0 || plen >= (int32_t)sizeof(c->port)) return 0;
            memcpy(c->port, url.data + colon + 1, (size_t)plen);
            c->port[plen] = 0;
            host_end      = colon;
        } else {
            snprintf(c->port, sizeof(c->port), "%s", c->plain ? "80" : "443");
        }
    }

    if (host_end - host_start >= FR_NET_HOST_MAX) return 0;
    memcpy(c->host, url.data + host_start, (size_t)(host_end - host_start));
    c->host[host_end - host_start] = 0;

    *out_path     = url.data + at;
    *out_path_len = url.len - at;
    if (*out_path_len <= 0) {
        *out_path     = "/";
        *out_path_len = 1;
    }
    return 1;
}

static int32_t fr_build_head(Fr_Conn *c, const Friend_Io_Request *req,
                             const char *path, int32_t path_len)
{
    int     n;
    int32_t at = 0;

    n = snprintf(c->head, (size_t)FR_NET_HEAD_BUDGET,
                 "%.*s %.*s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: friend-local/1\r\n"
                 "Accept: */*\r\n"
                 "Connection: close\r\n",
                 (int)req->method.len, req->method.data, (int)path_len, path,
                 c->host);
    if (n < 0 || n >= FR_NET_HEAD_BUDGET) return 0;
    at = n;

    if (req->headers.len > 0) {
        if (at + req->headers.len + 2 >= FR_NET_HEAD_BUDGET) return 0;
        memcpy(c->head + at, req->headers.data, (size_t)req->headers.len);
        at += req->headers.len;
        if (!(req->headers.len >= 2 && c->head[at - 1] == '\n')) {
            c->head[at++] = '\r';
            c->head[at++] = '\n';
        }
    }

    n = snprintf(c->head + at, (size_t)(FR_NET_HEAD_BUDGET - at),
                 "Content-Length: %ld\r\n\r\n", (long)req->body.len);
    if (n < 0 || at + n >= FR_NET_HEAD_BUDGET) return 0;
    at += n;

    c->head_len  = at;
    c->head_sent = 0;
    return 1;
}

#if defined(MSG_NOSIGNAL)
#define FR_NET_SEND_FLAGS MSG_NOSIGNAL
#else
#define FR_NET_SEND_FLAGS 0 /* Apple: SO_NOSIGPIPE on the socket instead */
#endif

/* mbedtls_net_send, except through send() rather than write(), so that a
 * peer that has gone is EPIPE rather than a signal that kills the
 * process. TLS's way out to the socket, and plain HTTP's. */
static int fr_net_send(void *ctx, const unsigned char *at, size_t len)
{
    ssize_t n = send(((mbedtls_net_context *)ctx)->fd, at, len,
                     FR_NET_SEND_FLAGS);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int32_t fr_conn_start_tls(Fr_Net *net, Fr_Conn *c)
{
    int rc;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    c->tls_live = 1;

    rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        fr_net_say(c->error, sizeof(c->error), "TLS setup failed", rc);
        return 0;
    }
    /* Verification is required. A friend's conversation is not something
     * to hand to whoever answered. */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &net->roots, 0);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &net->drbg);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc != 0) {
        fr_net_say(c->error, sizeof(c->error), "TLS setup failed", rc);
        return 0;
    }
    rc = mbedtls_ssl_set_hostname(&c->ssl, c->host);
    if (rc != 0) {
        fr_net_say(c->error, sizeof(c->error), "TLS hostname rejected", rc);
        return 0;
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->net, fr_net_send, mbedtls_net_recv, 0);
    return 1;
}

/* The websocket half is further down, next to the frame parser it
 * belongs with; these two are needed up here. */
static int32_t fr_build_ws_head(Fr_Net *net, Fr_Conn *c,
                                const Friend_Io_Request *req,
                                const char *path, int32_t path_len);
static int32_t fr_ws_queue(Fr_Conn *c, Fr_Net *net, uint64_t id,
                           int32_t opcode, const uint8_t *payload,
                           int32_t payload_len);

static void fr_net_fail(Friend_Context *fr, uint64_t id, const char *why)
{
    Friend_String message;
    message.data = why;
    message.len  = (int32_t)strlen(why);
    friend_io_failed(fr, id, message);
}

static int32_t fr_net_dispatch(Fr_Net *net, Friend_Context *fr,
                               const Friend_Io_Request *request)
{
    Fr_Conn    *c = 0;
    const char *path;
    int32_t     path_len;
    int32_t     i;
    int32_t     queued;
    int         rc;

    if (request->kind == FRIEND_IO_WS_SEND ||
        request->kind == FRIEND_IO_WS_CLOSE) {
        Fr_Conn *open = 0;
        for (i = 0; i < net->conn_count; ++i) {
            if (net->conns[i].ws && net->conns[i].id == request->channel) {
                open = &net->conns[i];
                break;
            }
        }
        if (!open) {
            friend_io_failed(fr, request->id,
                             FRIEND_LIT("that websocket is not open"));
            return 1;
        }
        if (request->kind == FRIEND_IO_WS_CLOSE) {
            /* Politeness, best effort: the frame is queued and the
             * socket goes when it has gone out. */
            fr_ws_queue(open, net, 0, 0x8, (const uint8_t *)"", 0);
            open->ws_closing = 1;
            friend_io_complete(fr, request->id, 0);
            return 1;
        }
        queued = fr_ws_queue(open, net, request->id,
                             (request->flags & FRIEND_IO_FLAG_BINARY) ? 0x2
                                                                      : 0x1,
                             (const uint8_t *)request->body.data,
                             request->body.len);
        if (queued == 0) return 0; /* backed up; again next poll */
        if (queued < 0) fr_net_fail(fr, request->id, open->error);
        return 1;
    }

    if (request->kind != FRIEND_IO_HTTPS &&
        request->kind != FRIEND_IO_WS_OPEN) {
        friend_io_failed(fr, request->id,
                         FRIEND_LIT("this transport makes HTTPS requests "
                                    "and opens websockets"));
        return 1;
    }

    for (i = 0; i < net->conn_count; ++i) {
        if (net->conns[i].state == FR_CONN_FREE) {
            c = &net->conns[i];
            break;
        }
    }
    if (!c) return 0; /* every connection is busy; ask again next poll */

    {
        char *head = c->head;
        memset(c, 0, sizeof(*c));
        c->head = head;
        mbedtls_net_init(&c->net);
    }
    c->id             = request->id;
    c->recv           = request->recv;
    c->recv_capacity  = request->recv_capacity;
    c->content_length = -1;
    c->body           = request->body;

    if (!fr_split_url(request->url, c, &path, &path_len) ||
        !(request->kind == FRIEND_IO_WS_OPEN
              ? fr_build_ws_head(net, c, request, path, path_len)
              : fr_build_head(c, request, path, path_len))) {
        c->state = FR_CONN_FREE;
        friend_io_failed(fr, request->id,
                         FRIEND_LIT("that URL could not be made into a "
                                    "request"));
        return 1;
    }

    /* The one blocking step. */
    rc = mbedtls_net_connect(&c->net, c->host, c->port,
                             MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        fr_net_say(net->error, sizeof(net->error),
                   "could not reach the server", rc);
        fr_conn_close(c);
        fr_net_fail(fr, request->id, net->error);
        return 1;
    }
    mbedtls_net_set_nonblock(&c->net);
#if defined(SO_NOSIGPIPE)
    {
        int one = 1;
        setsockopt(c->net.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif

    if (c->plain) {
        c->state = FR_CONN_SENDING;
        return 1;
    }
    if (!fr_conn_start_tls(net, c)) {
        snprintf(net->error, sizeof(net->error), "%s", c->error);
        fr_conn_close(c);
        fr_net_fail(fr, request->id, net->error);
        return 1;
    }
    c->state = FR_CONN_HANDSHAKE;
    return 1;
}

void fr_net_submit(Fr_Net *net, Friend_Context *fr,
                   const Friend_Io_Request *request)
{
    if (!net || !fr || !request) return;
    if (fr_net_dispatch(net, fr, request)) return;
    if (net->queue_count < net->queue_capacity) {
        net->queue[net->queue_count++] = *request;
        return;
    }
    friend_io_failed(fr, request->id,
                     FRIEND_LIT("the transport has more work than it can "
                                "hold: raise its connection count"));
}

/* Everything that was waiting for a connection and can now have one. */
static void fr_net_drain_queue(Fr_Net *net, Friend_Context *fr)
{
    int32_t kept = 0;
    int32_t i;

    for (i = 0; i < net->queue_count; ++i) {
        if (fr_net_dispatch(net, fr, &net->queue[i])) continue;
        net->queue[kept++] = net->queue[i];
    }
    net->queue_count = kept;
}

void fr_net_abandon(Fr_Net *net, uint64_t id)
{
    int32_t i;
    if (!net) return;
    for (i = 0; i < net->conn_count; ++i) {
        if (net->conns[i].state != FR_CONN_FREE &&
            net->conns[i].id == id) {
            net->conns[i].abandoned = 1;
        }
    }
    for (i = 0; i < net->queue_count; ++i) {
        if (net->queue[i].id != id) continue;
        net->queue[i] = net->queue[--net->queue_count];
        break;
    }
}

/* ------------------------------------------------------------------ *
 *  Reading and writing
 * ------------------------------------------------------------------ */

static int fr_conn_write(Fr_Conn *c, const unsigned char *at, size_t len)
{
    if (c->plain) return fr_net_send(&c->net, at, len);
    return mbedtls_ssl_write(&c->ssl, at, len);
}

static int fr_conn_read(Fr_Conn *c, unsigned char *at, size_t len)
{
    if (c->plain) {
        ssize_t n = read(c->net.fd, at, len);
        if (n >= 0) return (int)n;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return mbedtls_ssl_read(&c->ssl, at, len);
}

static int32_t fr_wants_write(const Fr_Conn *c)
{
    /* An abandoned websocket is written to once more, to close it: an
     * idle one would otherwise never be woken to notice. */
    if (c->state == FR_CONN_WS) {
        return c->send_count > 0 || c->ws_closing || c->abandoned;
    }
    return c->state == FR_CONN_SENDING || c->state == FR_CONN_HANDSHAKE;
}

/* The status line and the two headers that decide how the body is
 * framed. Returns 1 when the header block is complete, 0 when more is
 * needed, -1 when it is malformed. */
static int32_t fr_parse_head(Fr_Conn *c, int32_t *out_body_at)
{
    int32_t i;
    int32_t line_start = 0;
    int32_t end        = -1;

    for (i = 0; i + 3 < c->status_head_len; ++i) {
        if (c->status_head[i] == '\r' && c->status_head[i + 1] == '\n' &&
            c->status_head[i + 2] == '\r' && c->status_head[i + 3] == '\n') {
            end = i + 4;
            break;
        }
    }
    if (end < 0) {
        if (c->status_head_len >= FR_NET_HEAD_MAX) return -1;
        return 0;
    }
    *out_body_at = end;

    /* HTTP/1.1 200 OK */
    if (c->status_head_len < 12 ||
        memcmp(c->status_head, "HTTP/1.", 7) != 0) {
        return -1;
    }
    c->status = (c->status_head[9] - '0') * 100 +
                (c->status_head[10] - '0') * 10 + (c->status_head[11] - '0');

    for (i = 0; i < end; ++i) {
        if (c->status_head[i] != '\n') continue;
        {
            const char *line = c->status_head + line_start;
            int32_t     len  = i - line_start;
            line_start       = i + 1;
            if (len > 0 && line[len - 1] == '\r') --len;
            if (len <= 0) continue;

            if (len > 18 &&
                strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
                int32_t j;
                for (j = 18; j + 7 <= len; ++j) {
                    if (strncasecmp(line + j, "chunked", 7) == 0) {
                        c->chunked         = 1;
                        c->chunk_left      = 0;
                        c->chunk_seen_size = 0;
                        break;
                    }
                }
            } else if (len > 15 &&
                       strncasecmp(line, "Content-Length:", 15) == 0) {
                int64_t v  = 0;
                int32_t j  = 15;
                int32_t any = 0;
                while (j < len && (line[j] == ' ' || line[j] == '\t')) ++j;
                while (j < len && line[j] >= '0' && line[j] <= '9') {
                    v = v * 10 + (line[j] - '0');
                    ++j;
                    any = 1;
                }
                if (any) c->content_length = v;
            }
        }
    }
    return 1;
}

/* Hands body bytes to the friend, dechunking on the way if it has to.
 *
 * The friend's buffer is append-only for the life of the request: what
 * we have appended so far is body_received, and that is where the next
 * byte goes. Returns how much of `at` was consumed, or -1 when the
 * buffer is full — which for a streamed reply means the friend was not
 * given enough room, not that the answer is over.
 *
 * A partial chunk-size line is left unconsumed for the next read. */
static int32_t fr_deliver_body(Fr_Conn *c, Friend_Context *fr,
                               const uint8_t *at, int32_t len)
{
    int32_t used = 0;

    while (used < len && !c->chunk_done) {
        int32_t take;

        if (c->chunked && !c->chunk_seen_size) {
            int32_t line_end = used;
            int64_t size     = 0;
            int32_t digits   = 0;
            int32_t j;

            while (line_end < len && at[line_end] != '\n') ++line_end;
            if (line_end >= len) return used; /* the rest of the line later */

            for (j = used; j < line_end; ++j) {
                char ch = (char)at[j];
                int  d;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                else break; /* chunk extensions, and the carriage return */
                size = size * 16 + d;
                ++digits;
            }
            if (digits == 0) return -1;

            used               = line_end + 1;
            c->chunk_left      = size;
            c->chunk_seen_size = 1;
            if (size == 0) c->chunk_done = 1;
            continue;
        }

        if (c->chunked && c->chunk_left == 0) {
            /* The CRLF that closes a chunk's data. */
            if (at[used] == '\n') c->chunk_seen_size = 0;
            ++used;
            continue;
        }

        take = len - used;
        if (c->chunked && (int64_t)take > c->chunk_left) {
            take = (int32_t)c->chunk_left;
        }
        if (!c->chunked && c->content_length >= 0) {
            int64_t left = c->content_length - c->body_received;
            if ((int64_t)take > left) take = (int32_t)left;
            if (take <= 0) return used;
        }

        if (c->abandoned) {
            /* Nobody wants this any more; keep the framing straight and
             * throw the bytes away. */
            c->body_received += take;
            if (c->chunked) c->chunk_left -= take;
            used += take;
            continue;
        }

        if (!c->recv || (uint64_t)c->body_received + (uint64_t)take >
                            (uint64_t)c->recv_capacity) {
            return -1;
        }
        memcpy(c->recv + c->body_received, at + used, (size_t)take);
        if (friend_io_progress(fr, c->id, (uint32_t)take) != FRIEND_OK) {
            return -1;
        }
        c->body_received += take;
        if (c->chunked) c->chunk_left -= take;
        used += take;
    }
    return used;
}

/* ------------------------------------------------------------------ *
 *  Websockets
 *
 *  RFC 6455, the part of it two APIs use: an upgrade, masked frames out,
 *  whole messages in, and a reply to a ping. No extensions, no
 *  compression (we do not offer any), and no client pings — the friend
 *  has its own clock and closes a session that has gone quiet, which is
 *  the same decision made somewhere it can be reasoned about.
 * ------------------------------------------------------------------ */

#define FR_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* The sixteen random bytes a client offers, and what the server must
 * hash them into. Written into the connection so the answer can be
 * checked against it. */
static int32_t fr_ws_key(Fr_Net *net, Fr_Conn *c, char *out_key,
                         size_t out_size)
{
    unsigned char raw[16];
    unsigned char joined[64];
    unsigned char digest[20];
    size_t        len = 0;
    int32_t       n;

    if (mbedtls_ctr_drbg_random(&net->drbg, raw, sizeof(raw)) != 0) return 0;
    if (mbedtls_base64_encode((unsigned char *)out_key, out_size, &len, raw,
                              sizeof(raw)) != 0) {
        return 0;
    }
    out_key[len] = 0;

    n = snprintf((char *)joined, sizeof(joined), "%s%s", out_key, FR_WS_GUID);
    if (n < 0 || n >= (int32_t)sizeof(joined)) return 0;
    if (mbedtls_sha1(joined, (size_t)n, digest) != 0) return 0;
    if (mbedtls_base64_encode((unsigned char *)c->ws_accept,
                              sizeof(c->ws_accept) - 1, &len, digest,
                              sizeof(digest)) != 0) {
        return 0;
    }
    c->ws_accept[len] = 0;
    return 1;
}

static int32_t fr_build_ws_head(Fr_Net *net, Fr_Conn *c,
                                const Friend_Io_Request *req,
                                const char *path, int32_t path_len)
{
    char    key[32];
    int     n;
    int32_t at;

    if (!fr_ws_key(net, c, key, sizeof(key))) return 0;

    n = snprintf(c->head, (size_t)FR_NET_HEAD_BUDGET,
                 "GET %.*s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: friend-local/1\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "Sec-WebSocket-Key: %s\r\n",
                 (int)path_len, path, c->host, key);
    if (n < 0 || n >= FR_NET_HEAD_BUDGET) return 0;
    at = n;

    /* The friend's own headers: this is where the key goes. */
    if (req->headers.len > 0) {
        if (at + req->headers.len + 2 >= FR_NET_HEAD_BUDGET) return 0;
        memcpy(c->head + at, req->headers.data, (size_t)req->headers.len);
        at += req->headers.len;
        if (!(req->headers.len >= 2 && c->head[at - 1] == '\n')) {
            c->head[at++] = '\r';
            c->head[at++] = '\n';
        }
    }
    if (at + 2 >= FR_NET_HEAD_BUDGET) return 0;
    c->head[at++] = '\r';
    c->head[at++] = '\n';

    c->head_len  = at;
    c->head_sent = 0;
    return 1;
}

/* Is this the answer we asked for? A 101 with the hash of the key we
 * sent. Anything else — including a 200 with a polite explanation — is
 * not a websocket. */
static int32_t fr_ws_accepted(Fr_Conn *c)
{
    int32_t i;
    int32_t line_start = 0;

    if (c->status != 101) return 0;
    for (i = 0; i < c->status_head_len; ++i) {
        if (c->status_head[i] != '\n') continue;
        {
            const char *line = c->status_head + line_start;
            int32_t     len  = i - line_start;
            line_start       = i + 1;
            if (len > 0 && line[len - 1] == '\r') --len;
            if (len > 22 &&
                strncasecmp(line, "Sec-WebSocket-Accept:", 21) == 0) {
                int32_t j = 21;
                int32_t k = 0;
                while (j < len && (line[j] == ' ' || line[j] == '\t')) ++j;
                while (j < len && c->ws_accept[k] &&
                       line[j] == c->ws_accept[k]) {
                    ++j;
                    ++k;
                }
                return c->ws_accept[k] == 0 && (j >= len);
            }
        }
    }
    return 0;
}

/* Puts a frame in the queue. The payload is not copied — for a send it
 * is the friend's, which stays put until the request completes, and for
 * a control frame it is the connection's own.
 *
 * Returns 1, 0 when the queue is full, or -1 when there was no mask to
 * send it under: the connection is then closing, with the reason in its
 * error. */
static int32_t fr_ws_queue(Fr_Conn *c, Fr_Net *net, uint64_t id,
                           int32_t opcode, const uint8_t *payload,
                           int32_t payload_len)
{
    Fr_Ws_Send *s;
    int32_t     at = 0;

    if (c->send_count >= FR_WS_SENDS) return 0;
    s = &c->sends[c->send_count];
    memset(s, 0, sizeof(*s));
    /* A client masks every frame it sends, with a key nobody on the path
     * can predict (RFC 6455 section 10.3). The server does not mask. */
    if (mbedtls_ctr_drbg_random(&net->drbg, s->mask, sizeof(s->mask)) != 0) {
        snprintf(c->error, sizeof(c->error),
                 "the random number generator failed, so nothing more "
                 "can be sent on this websocket");
        c->ws_closing = 1;
        return -1;
    }
    s->id        = id;
    s->body.data = (const char *)payload;
    s->body.len  = payload_len;

    s->head[at++] = (uint8_t)(0x80u | (uint32_t)opcode); /* always final */
    if (payload_len < 126) {
        s->head[at++] = (uint8_t)(0x80u | (uint32_t)payload_len);
    } else if (payload_len < 65536) {
        s->head[at++] = (uint8_t)(0x80u | 126u);
        s->head[at++] = (uint8_t)((payload_len >> 8) & 0xFF);
        s->head[at++] = (uint8_t)(payload_len & 0xFF);
    } else {
        int32_t i;
        s->head[at++] = (uint8_t)(0x80u | 127u);
        for (i = 7; i >= 0; --i) {
            s->head[at++] =
                (uint8_t)(((uint64_t)payload_len >> (i * 8)) & 0xFFu);
        }
    }
    memcpy(s->head + at, s->mask, 4);
    at += 4;
    s->head_len = at;
    c->send_count += 1;
    return 1;
}

static void fr_ws_drop_first_send(Fr_Conn *c)
{
    int32_t i;
    for (i = 1; i < c->send_count; ++i) c->sends[i - 1] = c->sends[i];
    if (c->send_count > 0) c->send_count -= 1;
}

/* Writes what it can of the frames that are waiting. Returns 0 when the
 * connection broke. */
static int32_t fr_ws_write(Fr_Conn *c, Friend_Context *fr)
{
    while (c->send_count > 0) {
        Fr_Ws_Send *s = &c->sends[0];
        int         rc;

        while (s->head_sent < s->head_len) {
            rc = fr_conn_write(c, s->head + s->head_sent,
                               (size_t)(s->head_len - s->head_sent));
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return 1;
            }
            if (rc <= 0) return 0;
            s->head_sent += rc;
        }
        while (s->sent < s->body.len) {
            unsigned char masked[4096];
            int32_t       take = s->body.len - s->sent;
            int32_t       i;

            if (take > (int32_t)sizeof(masked)) take = (int32_t)sizeof(masked);
            for (i = 0; i < take; ++i) {
                int32_t at = s->sent + i;
                masked[i]  = (uint8_t)((uint8_t)s->body.data[at] ^
                                      s->mask[at & 3]);
            }
            rc = fr_conn_write(c, masked, (size_t)take);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return 1;
            }
            if (rc <= 0) return 0;
            s->sent += rc;
        }
        if (s->id != 0) friend_io_complete(fr, s->id, 0);
        fr_ws_drop_first_send(c);
    }
    return 1;
}

/* The connection is over: tell the friend once, both ways, and let the
 * slot that was holding the socket's buffer go. */
static void fr_ws_finish(Fr_Conn *c, Friend_Context *fr)
{
    int32_t  i;
    uint64_t id   = c->id;
    char     why[FR_NET_ERROR_MAX];
    int32_t  said = c->error[0] != 0;

    snprintf(why, sizeof(why), "%s", c->error);
    for (i = 0; i < c->send_count; ++i) {
        if (c->sends[i].id != 0) {
            friend_io_complete(fr, c->sends[i].id, 0);
        }
    }
    c->send_count = 0;
    c->ws         = 0;
    fr_conn_close(c);
    friend_ws_closed(fr, id);
    /* The open request is what has been holding the socket. It ends
     * with the reason when there was one — a close frame carries a code
     * and often a sentence, and losing that sentence is losing the only
     * explanation anybody is going to get. */
    if (said) {
        fr_net_fail(fr, id, why);
    } else {
        friend_io_complete(fr, id, 0);
    }
}

/* One message, whole, into the friend's hands. */
static void fr_ws_deliver(Fr_Conn *c, Friend_Context *fr)
{
    Friend_String message;
    message.data = (const char *)c->recv;
    message.len  = (int32_t)c->msg_len;
    friend_ws_recv(fr, c->id, message,
                   c->msg_binary ? (uint32_t)FRIEND_IO_FLAG_BINARY : 0u);
    c->msg_len = 0;
}

/* Feeds bytes off the wire through the frame parser. Returns 0 when the
 * connection should end. */
static int32_t fr_ws_feed(Fr_Conn *c, Fr_Net *net, Friend_Context *fr,
                          const uint8_t *at, int32_t len)
{
    int32_t i = 0;

    while (i < len) {
        /* The header first. Two bytes say how much more of it there is. */
        if (c->frame_need == 0 && c->frame_left == 0) {
            c->frame_len  = 0;
            c->frame_need = 2;
        }
        if (c->frame_len < c->frame_need) {
            int32_t take = c->frame_need - c->frame_len;
            if (take > len - i) take = len - i;
            memcpy(c->frame + c->frame_len, at + i, (size_t)take);
            c->frame_len += take;
            i += take;
            if (c->frame_len < c->frame_need) return 1;

            if (c->frame_need == 2) {
                int32_t length = c->frame[1] & 0x7F;
                int32_t masked = (c->frame[1] & 0x80) != 0;
                if (masked) {
                    snprintf(c->error, sizeof(c->error),
                             "the server masked a frame, which clients do "
                             "and servers must not");
                    return 0;
                }
                if (length == 126) c->frame_need = 4;
                else if (length == 127) c->frame_need = 10;
                if (c->frame_need > 2 && c->frame_len < c->frame_need) {
                    continue;
                }
            }

            /* The whole header is in. */
            {
                int32_t length = c->frame[1] & 0x7F;
                c->frame_fin     = (c->frame[0] & 0x80) != 0;
                c->frame_opcode  = c->frame[0] & 0x0F;
                c->frame_control = (c->frame_opcode & 0x08) != 0;
                if (length == 126) {
                    c->frame_left = ((int64_t)c->frame[2] << 8) |
                                    (int64_t)c->frame[3];
                } else if (length == 127) {
                    int32_t k;
                    c->frame_left = 0;
                    for (k = 0; k < 8; ++k) {
                        c->frame_left = (c->frame_left << 8) |
                                        (int64_t)c->frame[2 + k];
                    }
                } else {
                    c->frame_left = length;
                }
                c->frame_need = 0;
                c->ctl_len    = 0;
                if (c->frame_control && c->frame_left >= FR_WS_CONTROL_MAX) {
                    snprintf(c->error, sizeof(c->error),
                             "a control frame was longer than the protocol "
                             "allows");
                    return 0;
                }
                /* A new message, rather than the rest of one. */
                if (!c->frame_control && c->frame_opcode != 0) {
                    c->msg_len    = 0;
                    c->msg_binary = c->frame_opcode == 2;
                }
            }
        }

        /* Then the payload. */
        if (c->frame_left > 0) {
            int32_t take = len - i;
            if ((int64_t)take > c->frame_left) take = (int32_t)c->frame_left;
            if (take > 0) {
                if (c->frame_control) {
                    memcpy(c->ctl + c->ctl_len, at + i, (size_t)take);
                    c->ctl_len += take;
                } else if (c->abandoned) {
                    /* The friend let go of this request, and with it the
                     * buffer: what is still arriving goes nowhere. */
                } else {
                    if (c->msg_len + (uint32_t)take > c->recv_capacity) {
                        snprintf(c->error, sizeof(c->error),
                                 "a message was larger than the buffer kept "
                                 "for it");
                        return 0;
                    }
                    memcpy(c->recv + c->msg_len, at + i, (size_t)take);
                    c->msg_len += (uint32_t)take;
                }
                i += take;
                c->frame_left -= take;
            }
            if (c->frame_left > 0) return 1;
        }

        /* And what the frame meant. */
        if (c->frame_control) {
            if (c->frame_opcode == 0x8) { /* close */
                /* Two bytes of status and then, usually, a sentence. */
                int32_t code = 0;
                if (c->ctl_len >= 2) {
                    code = ((int32_t)c->ctl[0] << 8) | (int32_t)c->ctl[1];
                }
                if (code != 0 && code != 1000) {
                    snprintf(c->error, sizeof(c->error),
                             "the server closed the websocket (%d)%s%.*s",
                             code, c->ctl_len > 2 ? ": " : "",
                             c->ctl_len > 2 ? c->ctl_len - 2 : 0,
                             (const char *)c->ctl + 2);
                }
                c->ws_closing = 1;
                return 0;
            }
            if (c->frame_opcode == 0x9) { /* ping */
                memcpy(c->pong, c->ctl, (size_t)c->ctl_len);
                fr_ws_queue(c, net, 0, 0xA, c->pong, c->ctl_len);
            }
            /* 0xA is a pong; nothing asked for it, nothing to do. */
        } else if (c->frame_fin && !c->abandoned) {
            fr_ws_deliver(c, fr);
        }
    }
    return 1;
}

static void fr_conn_fail(Fr_Conn *c, Friend_Context *fr, const char *why)
{
    fr_net_fail(fr, c->id, why);
    fr_conn_close(c);
}

static void fr_conn_step(Fr_Net *net, Fr_Conn *c, Friend_Context *fr)
{
    unsigned char buffer[FR_NET_CHUNK_MAX];
    int           rc;

    switch (c->state) {
    case FR_CONN_HANDSHAKE:
        rc = mbedtls_ssl_handshake(&c->ssl);
        if (rc == 0) {
            c->state = FR_CONN_SENDING;
            return;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return;
        fr_net_say(c->error, sizeof(c->error),
                   "the server's certificate or TLS handshake was rejected",
                   rc);
        fr_conn_fail(c, fr, c->error);
        return;

    case FR_CONN_SENDING:
        while (c->head_sent < c->head_len) {
            rc = fr_conn_write(c, (const unsigned char *)c->head + c->head_sent,
                               (size_t)(c->head_len - c->head_sent));
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            if (rc <= 0) {
                fr_conn_fail(c, fr, "the connection closed while the "
                                    "request was being sent");
                return;
            }
            c->head_sent += rc;
        }
        while (c->body_sent < c->body.len) {
            rc = fr_conn_write(c,
                               (const unsigned char *)c->body.data +
                                   c->body_sent,
                               (size_t)(c->body.len - c->body_sent));
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            if (rc <= 0) {
                fr_conn_fail(c, fr, "the connection closed while the "
                                    "request was being sent");
                return;
            }
            c->body_sent += rc;
        }
        c->state = c->ws_accept[0] ? FR_CONN_WS_UPGRADE : FR_CONN_HEADERS;
        return;

    case FR_CONN_WS:
        if (c->abandoned && !c->ws_closing) {
            fr_ws_queue(c, net, 0, 0x8, (const uint8_t *)"", 0);
            c->ws_closing = 1;
        }
        if (!fr_ws_write(c, fr)) {
            fr_ws_finish(c, fr);
            return;
        }
        if (c->ws_closing && c->send_count == 0) {
            fr_ws_finish(c, fr);
            return;
        }
        for (;;) {
            rc = fr_conn_read(c, buffer, sizeof(buffer));
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc < 0) {
                fr_ws_finish(c, fr);
                return;
            }
            if (!fr_ws_feed(c, net, fr, buffer, rc)) {
                /* A close frame, or a frame we will not pretend to
                 * understand. Either way the session is over. */
                fr_ws_finish(c, fr);
                return;
            }
            if (!fr_ws_write(c, fr)) {
                fr_ws_finish(c, fr);
                return;
            }
        }

    case FR_CONN_WS_UPGRADE:
        for (;;) {
            int32_t room, take, body_at, parsed;

            rc = fr_conn_read(c, buffer, sizeof(buffer));
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            if (rc <= 0) {
                fr_conn_fail(c, fr, "the server closed before accepting the "
                                    "websocket");
                return;
            }
            room = FR_NET_HEAD_MAX - c->status_head_len;
            take = rc < room ? rc : room;
            memcpy(c->status_head + c->status_head_len, buffer, (size_t)take);
            c->status_head_len += take;

            body_at = 0;
            parsed  = fr_parse_head(c, &body_at);
            if (parsed < 0) {
                fr_conn_fail(c, fr, "the server's answer was not HTTP we "
                                    "understand");
                return;
            }
            if (parsed == 0) continue;
            if (!fr_ws_accepted(c)) {
                fr_conn_fail(c, fr, "the server would not upgrade the "
                                    "connection to a websocket");
                return;
            }

            c->ws    = 1;
            c->state = FR_CONN_WS;
            {
                /* Nothing was said yet; the friend is told the channel
                 * is live so it can start saying things. */
                Friend_String nothing;
                nothing.data = "";
                nothing.len  = 0;
                friend_ws_recv(fr, c->id, nothing,
                               (uint32_t)FRIEND_IO_FLAG_OPENED);
            }
            /* Frames that arrived in the same read as the headers. */
            if (body_at < c->status_head_len) {
                if (!fr_ws_feed(c, net, fr,
                                (const uint8_t *)c->status_head + body_at,
                                c->status_head_len - body_at)) {
                    fr_ws_finish(c, fr);
                    return;
                }
            }
            if (take < rc) {
                if (!fr_ws_feed(c, net, fr, buffer + take, rc - take)) {
                    fr_ws_finish(c, fr);
                    return;
                }
            }
            return;
        }

    case FR_CONN_HEADERS:
    case FR_CONN_BODY:
        for (;;) {
            rc = fr_conn_read(c, buffer, sizeof(buffer));
            if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
                rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                /* The server hung up. With no length and no chunking
                 * that is how a body ends; otherwise it is a truncated
                 * answer, and the friend is told the status anyway so it
                 * can decide. */
                if (c->state == FR_CONN_HEADERS) {
                    fr_conn_fail(c, fr, "the server closed before answering");
                    return;
                }
                friend_io_complete(fr, c->id, c->status);
                fr_conn_close(c);
                return;
            }
            if (rc < 0) {
                fr_net_say(c->error, sizeof(c->error),
                           "the connection broke while reading the answer",
                           rc);
                fr_conn_fail(c, fr, c->error);
                return;
            }

            {
                int32_t at = 0;
                if (c->state == FR_CONN_HEADERS) {
                    int32_t room = FR_NET_HEAD_MAX - c->status_head_len;
                    int32_t take = rc < room ? rc : room;
                    int32_t body_at = 0;
                    int32_t parsed;

                    memcpy(c->status_head + c->status_head_len, buffer,
                           (size_t)take);
                    c->status_head_len += take;

                    parsed = fr_parse_head(c, &body_at);
                    if (parsed < 0) {
                        fr_conn_fail(c, fr, "the server's answer was not "
                                            "HTTP we understand");
                        return;
                    }
                    if (parsed == 0) continue;

                    c->state = FR_CONN_BODY;
                    /* Whatever of the body arrived with the headers. */
                    if (body_at < c->status_head_len) {
                        int32_t extra = c->status_head_len - body_at;
                        if (fr_deliver_body(
                                c, fr, (const uint8_t *)c->status_head +
                                           body_at, extra) < 0) {
                            fr_conn_fail(c, fr, "the answer was larger than "
                                                "the buffer kept for it");
                            return;
                        }
                    }
                    at = take;
                    if (at >= rc) {
                        if (c->chunk_done) {
                            friend_io_complete(fr, c->id, c->status);
                            fr_conn_close(c);
                            return;
                        }
                        continue;
                    }
                }

                if (fr_deliver_body(c, fr, buffer + at, rc - at) < 0) {
                    fr_conn_fail(c, fr,
                                 "the answer was larger than the buffer "
                                 "kept for it");
                    return;
                }
            }

            if (c->chunk_done ||
                (c->content_length >= 0 &&
                 c->body_received >= c->content_length)) {
                friend_io_complete(fr, c->id, c->status);
                fr_conn_close(c);
                return;
            }
        }

    case FR_CONN_FREE:
        return;
    }
}

void fr_net_poll(Fr_Net *net, Friend_Context *fr, uint32_t wait_ms)
{
    struct pollfd fds[64];
    int32_t       which[64];
    int32_t       n = 0;
    int32_t       i;

    if (!net || !fr) return;
    if (net->queue_count > 0) fr_net_drain_queue(net, fr);

    for (i = 0; i < net->conn_count && n < 64; ++i) {
        Fr_Conn *c = &net->conns[i];
        if (c->state == FR_CONN_FREE) continue;
        fds[n].fd      = c->net.fd;
        fds[n].events  = (short)(fr_wants_write(c) ? (POLLIN | POLLOUT)
                                                   : POLLIN);
        fds[n].revents = 0;
        which[n]       = i;
        ++n;
    }
    if (wait_ms > 60000u) wait_ms = 60000u;
    if (poll(n > 0 ? fds : 0, (nfds_t)n, (int)wait_ms) <= 0) return;

    for (i = 0; i < n; ++i) {
        if (fds[i].revents == 0) continue;
        fr_conn_step(net, &net->conns[which[i]], fr);
    }
    if (net->queue_count > 0) fr_net_drain_queue(net, fr);
}
