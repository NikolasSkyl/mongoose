#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mongoose.h"

/* ---- static file server (original) ---- */

static const char *g_root = "docs";

static void donna_mongoose_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        struct mg_http_serve_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.root_dir = g_root;
        opts.mime_types = "xml=application/xml,rss=application/rss+xml";
        mg_http_serve_dir(c, hm, &opts);
    }
}

long donna_mongoose_serve(const char *root, long port) {
    struct mg_mgr mgr;
    char url[128];
    struct mg_connection *listener;

    if (root && root[0]) g_root = root;
    if (port <= 0) port = 1313;

    snprintf(url, sizeof(url), "http://0.0.0.0:%ld", port);

    mg_log_set(MG_LL_NONE);
    mg_mgr_init(&mgr);
    listener = mg_http_listen(&mgr, url, donna_mongoose_handler, NULL);
    if (listener == NULL) {
        mg_mgr_free(&mgr);
        return 1;
    }

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    return 0;
}

/* ---- dynamic HTTP server ---- */

/*
 * Server struct.  One request is buffered at a time.  The Donna event loop
 * calls donna_mongoose_accept (which blocks-by-polling until a request
 * arrives), processes the request, then calls donna_mongoose_respond to send
 * the reply before looping back to accept.
 *
 * Wire format returned by accept:
 *   method \x01 path \x01 query \x01 content_type \x01 body
 *
 * The body is the last field so it can contain arbitrary bytes without
 * escaping.  The preceding fields (method, path, query, content_type) are
 * HTTP tokens / URLs and never contain \x01.
 */
typedef struct {
    struct mg_mgr        mgr;
    struct mg_connection *pending_conn;
    char                 *wire;        /* heap-allocated request wire string */
    int                   has_pending;
    struct mg_connection *flush_conn;  /* connection we are waiting to drain */
    int                   flush_done;  /* set by handler when flush_conn drained/closed */
} donna_server;

static char *mgstr_dup(struct mg_str s) {
    char *p = malloc(s.len + 1);
    if (!p) return strdup("");
    memcpy(p, s.buf, s.len);
    p[s.len] = '\0';
    return p;
}

static void server_handler(struct mg_connection *c, int ev, void *ev_data) {
    donna_server *srv = (donna_server *)c->fn_data;

    /* flush tracking: signal when the response send-buffer has drained */
    if (c == srv->flush_conn) {
        if (ev == MG_EV_WRITE && c->send.len == 0) {
            srv->flush_done = 1;
        } else if (ev == MG_EV_CLOSE) {
            srv->flush_done = 1;
        }
    }

    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (srv->has_pending) {
        mg_http_reply(c, 503, "", "busy\n");
        return;
    }

    struct mg_str *ct_hdr = mg_http_get_header(hm, "Content-Type");
    char *content_type = ct_hdr ? mgstr_dup(*ct_hdr) : strdup("");

    /* method \x01 uri \x01 query \x01 content_type \x01 body */
    size_t total = hm->method.len + 1
                 + hm->uri.len   + 1
                 + hm->query.len + 1
                 + strlen(content_type) + 1
                 + hm->body.len  + 1;
    char *wire = malloc(total);
    if (!wire) {
        free(content_type);
        mg_http_reply(c, 500, "", "out of memory\n");
        return;
    }

    char *p = wire;
    memcpy(p, hm->method.buf, hm->method.len); p += hm->method.len; *p++ = '\x01';
    memcpy(p, hm->uri.buf,    hm->uri.len);    p += hm->uri.len;    *p++ = '\x01';
    memcpy(p, hm->query.buf,  hm->query.len);  p += hm->query.len;  *p++ = '\x01';
    size_t ctl = strlen(content_type);
    memcpy(p, content_type,   ctl);            p += ctl;             *p++ = '\x01';
    memcpy(p, hm->body.buf,   hm->body.len);   p += hm->body.len;   *p = '\0';

    free(content_type);
    srv->wire         = wire;
    srv->pending_conn = c;
    srv->has_pending  = 1;
    c->is_resp        = 1;  /* tell Mongoose the response is still being generated */
}

/* Start a server on the given port.  Returns a handle (non-zero) or 0 on error. */
intptr_t donna_mongoose_start(long port) {
    donna_server *srv = calloc(1, sizeof(donna_server));
    char url[128];

    if (port <= 0) port = 8080;
    snprintf(url, sizeof(url), "http://0.0.0.0:%ld", port);

    mg_log_set(MG_LL_NONE);
    mg_mgr_init(&srv->mgr);

    if (!mg_http_listen(&srv->mgr, url, server_handler, srv)) {
        mg_mgr_free(&srv->mgr);
        free(srv);
        return 0;
    }

    return (intptr_t)srv;
}

/*
 * Block (by polling) until the next HTTP request arrives.
 * Returns a heap-allocated wire string; the caller (Donna) owns it.
 */
char *donna_mongoose_accept(intptr_t handle) {
    donna_server *srv = (donna_server *)handle;
    if (!srv) return strdup("");

    while (!srv->has_pending) {
        mg_mgr_poll(&srv->mgr, 10);
    }

    char *wire = srv->wire;
    srv->wire = NULL;
    return wire;
}

/*
 * Send an HTTP response for the currently-pending request.
 * After this call the server is ready to accept the next request.
 */
long donna_mongoose_respond(
    intptr_t   handle,
    long       status,
    const char *content_type,
    const char *extra_headers,
    const char *body
) {
    donna_server *srv = (donna_server *)handle;
    if (!srv || !srv->pending_conn) return -1;

    if (!content_type || !*content_type)
        content_type = "text/plain; charset=utf-8";

    char headers[1024];
    if (extra_headers && *extra_headers) {
        snprintf(headers, sizeof(headers),
                 "Content-Type: %s\r\n%s", content_type, extra_headers);
    } else {
        snprintf(headers, sizeof(headers),
                 "Content-Type: %s\r\n", content_type);
    }

    struct mg_connection *conn = srv->pending_conn;
    srv->pending_conn = NULL;
    srv->has_pending  = 0;

    mg_http_reply(conn, (int)status, headers, "%s", body ? body : "");
    conn->is_resp = 0;  /* response generation complete; keep connection alive */

    /* flush: poll until the send buffer drains (tracked via handler to avoid
     * use-after-free if the client disconnects mid-flush) */
    srv->flush_conn = conn;
    srv->flush_done = 0;
    int i;
    for (i = 0; i < 50 && !srv->flush_done; i++) {
        mg_mgr_poll(&srv->mgr, 2);
    }
    srv->flush_conn = NULL;
    return 0;
}

/* Read a file from disk.
 * Returns 'O' + content on success, 'E' + message on failure.
 * Heap-allocated; the Donna runtime owns the result. */
char *donna_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *msg = "not found";
        size_t mlen = strlen(msg);
        char *r = malloc(1 + mlen + 1);
        if (!r) return strdup("Eout of memory");
        r[0] = 'E';
        memcpy(r + 1, msg, mlen + 1);
        return r;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return strdup("Eseek error"); }
    char *r = malloc(1 + sz + 1);
    if (!r) { fclose(f); return strdup("Eout of memory"); }
    r[0] = 'O';
    size_t n = fread(r + 1, 1, (size_t)sz, f);
    fclose(f);
    r[1 + n] = '\0';
    return r;
}

/* Shut down the server and free all resources. */
long donna_mongoose_stop(intptr_t handle) {
    donna_server *srv = (donna_server *)handle;
    if (!srv) return 0;
    if (srv->wire) free(srv->wire);
    mg_mgr_free(&srv->mgr);
    free(srv);
    return 0;
}

/* ---- HTTP client ---- */

/*
 * Wire format returned by donna_mongoose_fetch:
 *   status_code \x01 content_type \x01 body
 *
 * On error: "-1\x01\x01<reason>"
 */
typedef struct {
    int    done;
    int    status;
    char  *body;
    size_t body_len;
    char  *content_type;
    char  *url;           /* owned copy, needed inside handler */
    const char *method;
    const char *req_body;
    size_t      req_body_len;
    const char *extra_headers;
} fetch_state;

static void fetch_handler(struct mg_connection *c, int ev, void *ev_data) {
    fetch_state *fs = (fetch_state *)c->fn_data;

    if (ev == MG_EV_CONNECT) {
        struct mg_str host = mg_url_host(fs->url);
        const char *uri   = mg_url_uri(fs->url);
        mg_printf(c,
            "%s %s HTTP/1.1\r\n"
            "Host: %.*s\r\n"
            "Content-Length: %zu\r\n"
            "%s"
            "Connection: close\r\n"
            "\r\n",
            fs->method, uri,
            (int)host.len, host.buf,
            fs->req_body_len,
            fs->extra_headers ? fs->extra_headers : "");
        if (fs->req_body && fs->req_body_len > 0) {
            mg_send(c, fs->req_body, fs->req_body_len);
        }

    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        fs->status = mg_http_status(hm);

        fs->body = malloc(hm->body.len + 1);
        if (fs->body) {
            memcpy(fs->body, hm->body.buf, hm->body.len);
            fs->body[hm->body.len] = '\0';
            fs->body_len = hm->body.len;
        }

        struct mg_str *ct = mg_http_get_header(hm, "Content-Type");
        if (ct) {
            fs->content_type = mgstr_dup(*ct);
        }

        fs->done       = 1;
        c->is_draining = 1;

    } else if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE) {
        if (!fs->done) {
            fs->status = -1;
            fs->done   = 1;
        }
    }
}

/*
 * Perform a blocking HTTP request.
 *
 * method        — "GET", "POST", etc.
 * body          — request body (may be NULL / empty)
 * extra_headers — additional headers in "Key: Value\r\n" form (may be NULL)
 *
 * Returns a heap-allocated wire string the caller owns.
 */
char *donna_mongoose_fetch(
    const char *url,
    const char *method,
    const char *body,
    const char *extra_headers
) {
    struct mg_mgr mgr;
    fetch_state   fs;
    char         *wire;

    memset(&fs, 0, sizeof(fs));
    fs.url          = strdup(url ? url : "");
    fs.method       = (method && *method) ? method : "GET";
    fs.req_body     = body;
    fs.req_body_len = body ? strlen(body) : 0;
    fs.extra_headers = extra_headers;

    mg_log_set(MG_LL_NONE);
    mg_mgr_init(&mgr);

    if (!mg_http_connect(&mgr, url, fetch_handler, &fs)) {
        mg_mgr_free(&mgr);
        free(fs.url);
        return strdup("-1\x01\x01could not connect");
    }

    /* Poll up to 100 × 100 ms = 10 seconds */
    int i;
    for (i = 0; i < 100 && !fs.done; i++) {
        mg_mgr_poll(&mgr, 100);
    }

    mg_mgr_free(&mgr);
    free(fs.url);

    if (!fs.done || fs.status < 0) {
        if (fs.body) free(fs.body);
        if (fs.content_type) free(fs.content_type);
        return strdup("-1\x01\x01request timed out");
    }

    /* Build wire: status \x01 content_type \x01 body */
    const char *ct = fs.content_type ? fs.content_type : "";
    char status_str[16];
    snprintf(status_str, sizeof(status_str), "%d", fs.status);

    size_t slen = strlen(status_str);
    size_t clen = strlen(ct);
    size_t total = slen + 1 + clen + 1 + fs.body_len + 1;
    wire = malloc(total);
    if (wire) {
        char *p = wire;
        memcpy(p, status_str, slen); p += slen; *p++ = '\x01';
        memcpy(p, ct,         clen); p += clen; *p++ = '\x01';
        if (fs.body) memcpy(p, fs.body, fs.body_len);
        p[fs.body_len] = '\0';
    } else {
        wire = strdup("-1\x01\x01out of memory");
    }

    if (fs.body) free(fs.body);
    if (fs.content_type) free(fs.content_type);
    return wire;
}
