/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_STOCK_BROKER_H
#define DISKOS_AUDIO_STOCK_BROKER_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
enum { ASB_OPEN=1, ASB_READY, ASB_CLOSE, ASB_REAPED };
/* Separate inherited socketpair per attempt. No pathname or PID registration.
 * Fixed-size records, one regular input or one stream socket, CLOEXEC rights. */
static inline int asb_send(int socket, uint32_t generation, unsigned op, int fd){
    uint32_t words[4]={0x41534231,generation,op,0};
    if(!generation || op<ASB_OPEN || op>ASB_REAPED || ((op<=ASB_READY)!=(fd>=0))) return -1;
    struct iovec v={words,sizeof words}; struct msghdr m={.msg_iov=&v,.msg_iovlen=1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    if(fd>=0){
        memset(&control,0,sizeof control); m.msg_control=control.bytes; m.msg_controllen=sizeof control;
        struct cmsghdr *c=CMSG_FIRSTHDR(&m); c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS;
        c->cmsg_len=CMSG_LEN(sizeof fd); memcpy(CMSG_DATA(c),&fd,sizeof fd);
    }
    ssize_t n; do { n=sendmsg(socket,&m,MSG_NOSIGNAL|MSG_DONTWAIT); } while(n<0 && errno==EINTR);
    return n==sizeof words?0:-1;
}
static inline int asb_receive(int socket, uint32_t generation, unsigned *op, int *fd){
    uint32_t words[4]={0};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(16*sizeof(int))]; } control;
    struct iovec v={words,sizeof words}; struct msghdr m={.msg_iov=&v,.msg_iovlen=1,
        .msg_control=control.bytes,.msg_controllen=sizeof control};
    *fd=-1;
    ssize_t n; do { n=recvmsg(socket,&m,MSG_DONTWAIT|MSG_CMSG_CLOEXEC); } while(n<0 && errno==EINTR);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    if(n<=0) return -1;
    int count=0, valid=!(m.msg_flags&(MSG_TRUNC|MSG_CTRUNC));
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)){
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0)){ valid=0; continue; }
        size_t k=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
        for(size_t i=0;i<k;i++){
            int f; memcpy(&f,(char *)CMSG_DATA(c)+i*sizeof f,sizeof f);
            if(count++==0) *fd=f; else close(f);
        }
    }
    *op=words[2];
    if(n!=sizeof words || words[0]!=0x41534231 || words[1]!=generation || !generation ||
       words[3] || *op<ASB_OPEN || *op>ASB_REAPED || count!=(*op<=ASB_READY)) valid=0;
    if(count==1){
        struct stat st;
        if(fstat(*fd,&st) || (*op==ASB_OPEN?!S_ISREG(st.st_mode):!S_ISSOCK(st.st_mode))) valid=0;
    }
    if(!valid){ if(*fd>=0) close(*fd); *fd=-1; return -1; }
    return 1;
}
#endif
