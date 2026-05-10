#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mongoose.h"

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
