/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "remote_browse.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
static uint32_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void bytes(void *ctx, const char *b, size_t n){
    remote_disc *d = ctx; (void)remote_disc_feed(d, d->token, now_ms(), b, n);
}
static int order(const void *a, const void *b){
    const rd_receiver *x = a, *y = b; int c = strcmp(x->name, y->name);
    return c ? c : (int)x->protocol - (int)y->protocol;
}
static int failed(int rc){
    if(rc == CD_CANCEL) return rc;
    static const char *codes[] = { "CD_FOUND", "CD_NONE", "CD_MISSING", "CD_EXEC",
        "CD_DAEMON", "CD_TIMEOUT", "CD_CANCEL", "CD_ERROR" };
    fprintf(stderr, "Beam search failed: %s (%d)\n",
            rc >= CD_FOUND && rc <= CD_ERROR ? codes[rc] : "CD_UNKNOWN", rc);
    return rc;
}
int remote_browse_run(cast_disc_job *j, remote_disc *d, unsigned ms){
    static const char *types[] = { "_googlecast._tcp", "_airplay._tcp", "_raop._tcp" };
    memset(d, 0, sizeof *d);
    if(ms < 3 || ms > 30000 || remote_disc_begin(d, 1, now_ms(), ms) < 0) return failed(CD_ERROR);
    for(unsigned i = 0; i < 3; i++){
        int rc = cast_disc_run_lines(j, types[i], ms / 3, bytes, d);
        if(rc != CD_FOUND && rc != CD_NONE && rc != CD_TIMEOUT){ remote_disc_cancel(d, 1); return failed(rc); }
        /* An avahi stream may end without LF. Finalize that line without
         * ending the encompassing three-service attempt. */
        bytes(d, "\n", 1);
    }
    (void)remote_disc_finish(d, 1, now_ms());
    qsort(d->r, d->n, sizeof d->r[0], order); return d->n ? CD_FOUND : CD_NONE;
}
