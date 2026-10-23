/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bookmeta.h"
#include "sdio.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
struct bookmeta {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t wake;
    atomic_uint token, stop;
    int started, pending, mailbox, probe;
    bookmeta_key queued;
    uint64_t queued_generation;
    unsigned queued_token;
    bookmeta_snapshot mail;
    /* UI-only fields; worker never reads or writes these. */
    bookmeta_snapshot current;
    uint64_t serial;
    unsigned dispatched;
    int dirty;
};
static int same_key(const bookmeta_key *a,const bookmeta_key *b){
    return a->playback_generation==b->playback_generation &&
        a->library_revision==b->library_revision && a->media_generation==b->media_generation &&
        !strcmp(a->path,b->path);
}
static void failure(bookmeta_snapshot *c,book_meta_outcome_t outcome){
    c->result=(book_meta_result_t){outcome,outcome,0};
    memset(c->chapters,0,sizeof c->chapters); c->narrator[0]=0;
}
static int same_file(const struct stat *a,const struct stat *b){
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec && a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static void *worker(void *ctx){
    bookmeta *s=ctx;
    for(;;){
        pthread_mutex_lock(&s->mu);
        while(!s->pending && !s->probe && !atomic_load(&s->stop)) pthread_cond_wait(&s->wake,&s->mu);
        if(atomic_load(&s->stop)){pthread_mutex_unlock(&s->mu); break;}
        int pending=s->pending; s->probe=0;
        if(!pending){
            pthread_mutex_unlock(&s->mu);
            sd_io_check_media();
            continue;
        }
        bookmeta_snapshot c={.key=s->queued,.request_generation=s->queued_generation,
                             .state=BOOKMETA_COMPLETE};
        unsigned token=s->queued_token; s->pending=0;
        pthread_mutex_unlock(&s->mu);
        if(token!=atomic_load(&s->token)) continue;
        /* No mailbox lock spans admission, stat, allocation, reader I/O or close.
         * Outer lease protects stat and the complete identity check. The existing
         * combined reader also owns a nested lease until its descriptor closes. */
        failure(&c,BOOK_META_ADMISSION_REFUSED);
        sd_io_check_media();
        if(c.key.media_generation==sd_io_generation() && sd_io_begin()){
            struct stat before,after;
            failure(&c,BOOK_META_READ_FAILURE);
            if(stat(c.key.path,&before)==0 && S_ISREG(before.st_mode)){
                c.result=scan_read_book_meta(c.key.path,c.chapters,BOOKMETA_CHAPTERS,
                                            c.narrator,sizeof c.narrator);
                if(stat(c.key.path,&after)!=0 || !same_file(&before,&after)){
                    c.state=BOOKMETA_FILE_CHANGED; failure(&c,BOOK_META_READ_FAILURE);
                }
            }
            sd_io_check_media();
            if(!sd_io_allowed() || c.key.media_generation!=sd_io_generation()) failure(&c,BOOK_META_ADMISSION_REFUSED);
            sd_io_end();
        }
        if(c.result.chapters!=BOOK_META_OK){
            c.result.count=0; memset(c.chapters,0,sizeof c.chapters);
        }
        if(c.result.narrator!=BOOK_META_OK) c.narrator[0]=0;
        pthread_mutex_lock(&s->mu);
        if(token==atomic_load(&s->token) && !atomic_load(&s->stop)){
            s->mail=c; s->mailbox=1;
        }
        pthread_mutex_unlock(&s->mu);
    }
    return NULL;
}
bookmeta *bookmeta_create(void){
    bookmeta *s=calloc(1,sizeof *s); if(!s) return NULL;
    if(pthread_mutex_init(&s->mu,NULL)){free(s); return NULL;}
    if(pthread_cond_init(&s->wake,NULL)){pthread_mutex_destroy(&s->mu); free(s); return NULL;}
    atomic_init(&s->token,0); atomic_init(&s->stop,0);
    s->started=pthread_create(&s->thread,NULL,worker,s)==0;
    return s;
}
void bookmeta_destroy(bookmeta *s){
    if(!s) return;
    atomic_store(&s->stop,1); atomic_fetch_add(&s->token,1);
    pthread_mutex_lock(&s->mu); pthread_cond_signal(&s->wake); pthread_mutex_unlock(&s->mu);
    if(s->started) pthread_join(s->thread,NULL);
    pthread_cond_destroy(&s->wake); pthread_mutex_destroy(&s->mu); free(s);
}
static void dispatch(bookmeta *s){
    unsigned token=atomic_load(&s->token);
    if(s->current.state!=BOOKMETA_PENDING || s->dispatched==token) return;
    if(pthread_mutex_trylock(&s->mu)) return;
    s->queued=s->current.key; s->queued_generation=s->current.request_generation;
    s->queued_token=token; s->pending=1; s->dispatched=token;
    pthread_cond_signal(&s->wake); pthread_mutex_unlock(&s->mu);
}
static void reset(bookmeta *s,const bookmeta_key *key){
    atomic_fetch_add(&s->token,1);
    s->current=(bookmeta_snapshot){.request_generation=++s->serial}; s->dirty=1;
    if(key){
        s->current.key=*key;
        s->current.state=s->started ? BOOKMETA_PENDING : BOOKMETA_WORKER_FAILURE;
        failure(&s->current,BOOK_META_READ_FAILURE);
        dispatch(s);
    }
}
int bookmeta_request(bookmeta *s,const bookmeta_key *key,int is_book){
    if(!s) return -1;
    if(!is_book || !key || !key->path[0]){
        if(s->current.state!=BOOKMETA_NONE) reset(s,NULL);
        return 0;
    }
    if(key->path[0]!='/' || !memchr(key->path,0,sizeof key->path)) return -1;
    if(s->current.state!=BOOKMETA_NONE && same_key(key,&s->current.key)) return 0;
    reset(s,key); return 0;
}
int bookmeta_retry(bookmeta *s){
    if(!s || s->current.state==BOOKMETA_NONE) return -1;
    bookmeta_key key=s->current.key;
    if(!s->started) s->started=pthread_create(&s->thread,NULL,worker,s)==0;
    reset(s,&key); return 0;
}
int bookmeta_poll(bookmeta *s){
    if(!s) return 0;
    if(!pthread_mutex_trylock(&s->mu)){
        /* Polling is draw-gated by main; no mount probes are scheduled in darkness. */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
        static _Thread_local uint64_t last_probe;
        uint64_t ms=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
        if(ms-last_probe>=200){s->probe=1; last_probe=ms; pthread_cond_signal(&s->wake);}
        if(s->mailbox){
            if(s->current.state==BOOKMETA_PENDING &&
               s->mail.request_generation==s->current.request_generation &&
               same_key(&s->mail.key,&s->current.key)){
                s->current=s->mail; s->dirty=1;
            }
            s->mailbox=0;
        }
        pthread_mutex_unlock(&s->mu);
    }
    dispatch(s);
    int dirty=s->dirty; s->dirty=0; return dirty;
}
const bookmeta_snapshot *bookmeta_snapshot_get(const bookmeta *s){return s ? &s->current : NULL;}
