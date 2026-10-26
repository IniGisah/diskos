/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Private implementation included by audio_decode.c. No media path crosses exec. */
#ifndef DISKOS_AUDIO_STOCK_CLIENT_H
#define DISKOS_AUDIO_STOCK_CLIENT_H
#include "audio_stock_wire.h"
#include "audio_stock_process.h"
#include "audio_stock_broker.h"
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
static int stopped(audio_decode *d);
static int64_t stock_now(void){
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)) return -1;
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
/* Worker-only, set before its first decoder. Other threads retain direct ownership. */
static _Thread_local int stock_broker_fd=-1;
static _Thread_local uint32_t stock_broker_generation;
void audio_decode_stock_supervise(int fd, uint32_t generation){
    stock_broker_fd=fd; stock_broker_generation=generation;
}
static int stock_broker_exchange(audio_decode *d, unsigned op, int input, int *output){
    if(asb_send(stock_broker_fd,stock_broker_generation,op,input)) return -1;
    int64_t end=stock_now()+10000;
    for(;;){
        unsigned reply; int fd=-1;
        int n=asb_receive(stock_broker_fd,stock_broker_generation,&reply,&fd);
        if(n){
            if(n>0 && reply==(op==ASB_OPEN?ASB_READY:ASB_REAPED)){
                if(output) *output=fd; else if(fd>=0) close(fd);
                return 0;
            }
            if(fd>=0) close(fd);
            return -1;
        }
        /* Close must await ownership confirmation even after cancellation.
         * On failure the supervisor still retains the child and SD lease. */
        if(stock_now()>=end || (op==ASB_OPEN && stopped(d))) return -1;
        struct pollfd p={stock_broker_fd,POLLIN,0};
        if(poll(&p,1,20)<0 && errno!=EINTR) return -1;
    }
}
static ad_result stock_transfer(audio_decode *d, void *buf, size_t n, int writing, int64_t end){
    unsigned char *p = buf;
    while(n){
        if(stopped(d)) return d->error;
        int64_t now = stock_now();
        if(now < 0 || now >= end) return d->error = AD_DECODE;
        struct pollfd pollfd = {d->stock_fd,writing ? POLLOUT : POLLIN,0};
        int r = poll(&pollfd,1,20);
        if(r < 0 && errno == EINTR) continue;
        if(r < 0) return d->error = AD_IO;
        if(!r) continue;
        struct iovec iov={p,n}; struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1};
        ssize_t got = writing ? sendmsg(d->stock_fd,&msg,MSG_NOSIGNAL|MSG_DONTWAIT) : recvmsg(d->stock_fd,&msg,MSG_DONTWAIT);
        if(got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if(got <= 0) return d->error = AD_DECODE;
        p += got; n -= (size_t)got;
    }
    return AD_OK;
}
static ad_result stock_header(audio_decode *d, unsigned capacity, size_t *count, int initial, int64_t end){
    unsigned char h[AS_HEADER];
    ad_result r = stock_transfer(d,h,sizeof h,0,end);
    if(r) return r;
    unsigned status=as_get32(h+4), rate=as_get32(h+8), channels=as_get32(h+12), bits=as_get32(h+16), n=as_get32(h+20);
    if(as_get32(h)!=AS_MAGIC || status>AD_DECODE || n>capacity || (status && n) ||
       (initial && n) || (status==AD_OK && !initial && capacity && !n)) return d->error=AD_DECODE;
    if(initial && status) return d->error=(ad_result)status;
    if(rate<8000 || rate>AS_RATE_MAX || channels<1 || channels>2 || bits>32 ||
       (!initial && (rate!=d->info.rate || channels!=d->info.channels || bits!=d->info.bits || as_get64(h+24)!=d->info.frames))) return d->error=AD_DECODE;
    if(initial) d->info=(ad_info){AD_STOCK,rate,channels,bits,as_get64(h+24)};
    *count=n;
    if(status!=AD_OK && status!=AD_EOF && status!=AD_ARGUMENT) d->error=(ad_result)status;
    return (ad_result)status;
}
static void stock_close(audio_decode *d){
    if(d->stock_fd>=0){ close(d->stock_fd); d->stock_fd=-1; }
    if(d->stock_owned){
        (void)stock_broker_exchange(d,ASB_CLOSE,-1,NULL); d->stock_owned=0;
    }
    if(d->stock_pid>0){
        kill(d->stock_pid,SIGKILL);
        while(waitpid(d->stock_pid,NULL,0)<0 && errno==EINTR) {}
        d->stock_pid=0;
    }
}
static ad_result stock_open(audio_decode *d){
    if(stopped(d)) return d->error;
    if(d->limit!=AUDIO_DECODE_HEAP_MAX) return AD_FORMAT;
    if(stock_broker_fd>=0){
        if(stock_broker_exchange(d, ASB_OPEN, d->fd, &d->stock_fd)) return AD_DECODE;
        d->stock_owned=1;
    } else {
        d->stock_pid=stock_process_spawn(d->fd,&d->stock_fd);
        if(d->stock_pid<0){
            /* A failed private copy is retryable, never a verdict on the file. */
            ad_result r=d->stock_pid==STOCK_SPAWN_NONE?AD_FORMAT:d->stock_pid==STOCK_SPAWN_UNPREPARED?AD_UNAVAILABLE:AD_IO;
            d->stock_pid=0; return r;
        }
    }
    unsigned char ready[AS_HEADER], expected[AS_HEADER] = {0};
    as_put32(expected, AS_MAGIC);
    as_put32(expected+4, AD_FORMAT);
    as_put32(expected+8, AS_READY);
    ad_result r=stock_transfer(d,ready,sizeof ready,0,stock_now()+10000);
    unsigned char failed[AS_HEADER] = {0};
    as_put32(failed, AS_MAGIC); as_put32(failed + 8, AS_EXEC_FAILED);
    if(!r && !memcmp(ready,failed,sizeof ready)){
        stock_close(d); return d->error = AD_UNAVAILABLE;
    }
    if(r || memcmp(ready,expected,sizeof ready)){
        if(r == AD_DECODE || !r) r = d->error = AD_FORMAT;
        stock_close(d); return r;
    }
    size_t n=0; r=stock_header(d,0,&n,1,stock_now()+10000);
    if(r){ stock_close(d); return r; }
    atomic_store(&stock_available,1);
    d->opened=1; return AD_OK;
}
static ad_result stock_request(audio_decode *d, unsigned op, unsigned capacity, uint64_t frame, size_t *n, int64_t end){
    unsigned char q[AS_REQUEST]={0};
    as_put32(q,op); as_put32(q+4,capacity); as_put64(q+8,frame);
    ad_result r=stock_transfer(d,q,sizeof q,1,end);
    return r ? r : stock_header(d,capacity,n,0,end);
}
static ad_result stock_read(audio_decode *d, float *pcm, size_t capacity, size_t *n){
    int64_t end=stock_now()+10000;
    ad_result r=stock_request(d,AS_READ,(unsigned)capacity,0,n,end);
    if(r) return r;
    unsigned char raw[AS_MAX_FRAMES*2*4];
    r=stock_transfer(d,raw,*n*d->info.channels*4,0,end);
    if(r){ *n=0; return r; }
    for(size_t i=0;i<*n*d->info.channels;i++){
        uint32_t word=as_get32(raw+4*i); float v; memcpy(&v,&word,4);
        if(!isfinite(v)){ *n=0; return d->error=AD_DECODE; }
        pcm[i]=v;
    }
    return AD_OK;
}
#endif
