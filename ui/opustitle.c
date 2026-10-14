/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "opustitle.h"
#include <stdio.h>
#include <errno.h>
/* Capture the failing parser operation before callbacks/free/stat/close can
 * change errno. Thread-local storage keeps independent readers isolated. */
static _Thread_local int ot_io_error;
static size_t ot_read(void *p, size_t size, size_t n, FILE *f){
    errno=0;
    size_t got=fread(p,size,n,f);
    if(got!=n) ot_io_error=errno;
    return got;
}
static int ot_seek(FILE *f, off_t off, int whence){
    int r=fseeko(f,off,whence);
    if(r) ot_io_error=errno;
    return r;
}
static off_t ot_tell(FILE *f){
    off_t r=ftello(f);
    if(r<0) ot_io_error=errno;
    return r;
}
#pragma push_macro("fread")
#pragma push_macro("fseeko")
#pragma push_macro("ftello")
#undef fread
#define fread ot_read
#undef fseeko
#define fseeko ot_seek
#undef ftello
#define ftello ot_tell
#include "audiodur.h"
#pragma pop_macro("ftello")
#pragma pop_macro("fseeko")
#pragma pop_macro("fread")

static int ot_retryable(int error){
    return error==EINTR || error==EAGAIN || error==EWOULDBLOCK || error==EIO;
}
#include <sys/stat.h>
#include <unistd.h>

static int ot_same(const struct stat *a, const struct stat *b){
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
           a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec;
}
/* Qualify content before ad_opus (which deliberately maps malformed/non-Opus to 0).
 * For Ogg, only a CRC-checked, complete first BOS packet with a different codec
 * signature proves NOT_APPLICABLE. Unknown/damaged Ogg stays an error. */
static int ot_format(FILE *f, ad_cancel_fn cancel, void *ctx){
    unsigned char page[27+255+65025];
    int r=ad_opus_read(f,page,4,cancel,ctx);
    if(r!=1) return r;
    if(memcmp(page,"OggS",4)) return 2;
    r=ad_opus_read(f,page+4,23,cancel,ctx);
    if(r!=1) return r;
    unsigned ns=page[26];
    if(page[4] || page[5]!=2 || !ns || ad_le32(page+18) ||
       ad_le32(page+6) || ad_le32(page+10)) return 0;
    r=ad_opus_read(f,page+27,ns,cancel,ctx);
    if(r!=1) return r;
    size_t size=27+ns, packet=0;
    for(unsigned i=0;i<ns;i++){ size+=page[27+i]; packet+=page[27+i];
        if(page[27+i]!=255 && i+1!=ns) return 0;
    }
    if(page[26+ns]==255) return 0;
    r=ad_opus_read(f,page+27+ns,size-27-ns,cancel,ctx);
    if(r!=1) return r;
    if(ad_ogg_crc(page,size)!=ad_le32(page+22)) return 0;
    const unsigned char *p=page+27+ns;
    if(packet>=8 && !memcmp(p,"OpusHead",8)) return 1;
    if(packet>=7 && !memcmp(p,"\x01vorbis",7)) return 2;
    return 0;
}

opus_title_result opus_title_read(const char *path, int (*cancel)(void *), void *ctx){
    opus_title_result out={.outcome=OPUS_TITLE_ERROR,.reason=OPUS_TITLE_IO};
    struct stat before, after, named;
    unsigned char *tags=NULL;
    uint32_t len=0;
    if(cancel && cancel(ctx)){ out.reason=OPUS_TITLE_CANCELLED; return out; }
    FILE *f=path ? fopen(path,"rb") : NULL;
    if(!f){ out.retryable=path && ot_retryable(errno); return out; }
    if(fstat(fileno(f),&before)){ out.retryable=ot_retryable(errno); goto done; }
    if(!S_ISREG(before.st_mode)) goto done;
    ot_io_error=0;
    int format=ot_format(f,cancel,ctx);
    if(format==2){ out.outcome=OPUS_TITLE_NOT_APPLICABLE; out.reason=OPUS_TITLE_NONE; }
    else {
        long long r=format==1 ? ad_opus(f,&tags,&len,cancel,ctx) : format;
        if(r==1){
            int title=ad_opus_title(tags,len,out.title,sizeof out.title,cancel,ctx);
            if(title==AD_TITLE_FOUND || title==AD_TITLE_ABSENT){
                out.outcome=title==AD_TITLE_FOUND ? OPUS_TITLE_FOUND : OPUS_TITLE_ABSENT;
                out.reason=OPUS_TITLE_NONE;
            } else out.reason=title==AD_TITLE_CANCELLED ? OPUS_TITLE_CANCELLED :
                              title==AD_TITLE_VALUE ? OPUS_TITLE_VALUE : OPUS_TITLE_MALFORMED;
        } else out.reason=r==AD_CANCELLED ? OPUS_TITLE_CANCELLED :
                          r==AD_DEFERRED ? OPUS_TITLE_LIMIT : r<0 ? OPUS_TITLE_IO : OPUS_TITLE_MALFORMED;
    }
    if(out.reason==OPUS_TITLE_IO) out.retryable=ot_retryable(ot_io_error);
    if(cancel && cancel(ctx)){ out.outcome=OPUS_TITLE_ERROR; out.reason=OPUS_TITLE_CANCELLED; }
    if(fstat(fileno(f),&after)) { out.outcome=OPUS_TITLE_ERROR; out.reason=OPUS_TITLE_IO; out.retryable=ot_retryable(errno); }
    else if(!ot_same(&before,&after) || stat(path,&named) || !ot_same(&before,&named)){
        out.outcome=OPUS_TITLE_ERROR; out.reason=OPUS_TITLE_CHANGED;
    }
done:
    free(tags);
    if(fclose(f) && out.outcome!=OPUS_TITLE_ERROR){ out.outcome=OPUS_TITLE_ERROR; out.reason=OPUS_TITLE_IO; out.retryable=ot_retryable(errno); }
    if(out.outcome!=OPUS_TITLE_ERROR || out.reason!=OPUS_TITLE_IO) out.retryable=0;
    if(out.outcome!=OPUS_TITLE_FOUND) out.title[0]=0;
    return out;
}
