/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_STOCK_PROCESS_H
#define DISKOS_AUDIO_STOCK_PROCESS_H
#include "audio_stock_wire.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
/* stock_process_spawn failures. No pathname is ever executed: the only helper
 * is the one embedded in this program (audio_stock_image.S), and only a
 * private, verified copy of it runs. */
#define STOCK_SPAWN_FAILED (-1)     /* descriptors, signal disposition or fork: retryable */
#define STOCK_SPAWN_UNPREPARED (-2) /* the private helper copy could not be made or verified: retryable */
#define STOCK_SPAWN_NONE (-3)       /* this build carries no helper: no stock backend */
#define AUDIO_STOCK_IMAGE_MAX (256u * 1024u)
#define AUDIO_STOCK_IMAGE_MAGIC "DADEC001"
#ifndef AUDIO_STOCK_PRIVATE_DIR
#define AUDIO_STOCK_PRIVATE_DIR "/tmp"
#endif
#ifdef AUDIO_STOCK_EMBEDDED
#ifndef MBEDTLS_CONFIG_FILE
#error "the embedded stock helper is verified with mbedtls_sha256"
#endif
#include "mbedtls/sha256.h"
/* Generated from the finished helper (tools/embed_audiodec.py): the image in its
 * own read-only section, and 52 descriptor bytes: magic[8], length[4], zero[4],
 * SHA-256[32], then the length the assembler actually included[4]. */
extern const unsigned char diskos_audiodec_image[], diskos_audiodec_image_end[], diskos_audiodec_desc[];
/* The helper must be an executable for the CPU this program runs on. */
#if defined(__mips__) && defined(__MIPSEL__) && !defined(__mips64)
#define STOCK_ELF_CLASS 1
#define STOCK_ELF_MACHINE 8 /* EM_MIPS */
#elif defined(__x86_64__)
#define STOCK_ELF_CLASS 2
#define STOCK_ELF_MACHINE 62 /* EM_X86_64, host tests */
#elif defined(__aarch64__)
#define STOCK_ELF_CLASS 2
#define STOCK_ELF_MACHINE 183 /* EM_AARCH64, host tests */
#else
#define STOCK_ELF_CLASS 0
#define STOCK_ELF_MACHINE 0 /* unknown target: nothing ever matches */
#endif
static inline unsigned stock_get16(const unsigned char *p){ return (unsigned)p[0] | (unsigned)p[1] << 8; }
/* ELF identity: this target's class and machine, little-endian, current version, executable or PIE. */
static inline int stock_helper_elf(const unsigned char *h, size_t n){
    size_t header = STOCK_ELF_CLASS == 2 ? 64 : 52;
    if(!STOCK_ELF_MACHINE || n < header || memcmp(h, "\177ELF", 4) || h[4] != STOCK_ELF_CLASS || h[5] != 1 || h[6] != 1) return 0;
    unsigned type = stock_get16(h + 16);
    return (type == 2 || type == 3) && stock_get16(h + 18) == STOCK_ELF_MACHINE;
}
static inline int stock_write_all(int fd, const unsigned char *p, size_t n){
    while(n){
        ssize_t w = write(fd, p, n);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0 || (size_t)w > n) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
/* Length, ELF identity and SHA-256 of what will actually be executed: the copy
 * read back through its read-only descriptor after the writer was closed. */
static inline int stock_helper_verify(int fd, size_t size, const unsigned char *digest){
    struct stat st; unsigned char buf[4096], head[64], sum[32]; size_t got = 0, have = 0;
    if(fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink || (st.st_mode & 07777) != 0500 ||
       st.st_size < 0 || (uint64_t)st.st_size != size) return -1;
    mbedtls_sha256_context c; mbedtls_sha256_init(&c);
    int bad = mbedtls_sha256_starts(&c, 0) != 0;
    while(!bad){
        ssize_t r = pread(fd, buf, sizeof buf, (off_t)got);
        if(r < 0 && errno == EINTR) continue;
        if(r < 0 || (size_t)r > size - got){ bad = 1; break; }
        if(!r) break;
        for(ssize_t i = 0; i < r && have < sizeof head; i++) head[have++] = buf[i];
        bad = mbedtls_sha256_update(&c, buf, (size_t)r) != 0; got += (size_t)r;
    }
    if(!bad) bad = mbedtls_sha256_finish(&c, sum) != 0;
    mbedtls_sha256_free(&c);
    return bad || got != size || !stock_helper_elf(head, have) || memcmp(sum, digest, sizeof sum) ? -1 : 0;
}
/* Report an execution failure separately from the helper's backend refusal. */
static inline void stock_process_unavailable(int fd){
    unsigned char h[AS_HEADER] = {0};
    as_put32(h, AS_MAGIC); as_put32(h + 8, AS_EXEC_FAILED);
    size_t done = 0;
    while(done < sizeof h){
        ssize_t n = send(fd, h + done, sizeof h - done, MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        done += (size_t)n;
    }
    _exit(127);
}
/* A read-only, close-on-exec descriptor of a private copy of the embedded helper:
 * created on AUDIO_STOCK_PRIVATE_DIR and unlinked at once, written with checked
 * writes, writer closed, then verified. -1 on any failure; nothing is left named. */
static inline int stock_helper_prepare(void){
    const unsigned char *image = diskos_audiodec_image, *desc = diskos_audiodec_desc;
    size_t size = (size_t)(diskos_audiodec_image_end - diskos_audiodec_image);
    if(memcmp(desc, AUDIO_STOCK_IMAGE_MAGIC, 8) || as_get32(desc + 8) != size || as_get32(desc + 12) ||
       as_get32(desc + 48) != size || !size || size > AUDIO_STOCK_IMAGE_MAX || !stock_helper_elf(image, size)) return -1;
    char path[] = AUDIO_STOCK_PRIVATE_DIR "/.diskos-audiodec-XXXXXX";
    int writer = mkostemp(path, O_CLOEXEC);
    if(writer < 0) return -1;
    int reader = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int named = unlink(path); /* private from here on: nobody can open it by name */
    struct stat a, b;
    if(reader < 0 || named || fstat(writer, &a) || fstat(reader, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino ||
       fchmod(writer, 0500) || stock_write_all(writer, image, size)){
        close(writer); if(reader >= 0) close(reader);
        return -1;
    }
    if(close(writer) || stock_helper_verify(reader, size, desc + 16)){ close(reader); return -1; }
    return reader;
}
#endif
/* The caller owns the returned child until waitpid confirms its reap. Negative
 * results are the STOCK_SPAWN_* reasons; no child exists then. The child keeps
 * control on fd 3, media on fd 4 and the verified copy on close-on-exec fd 5. */
static inline pid_t stock_process_spawn(int input, int *output){
#ifndef AUDIO_STOCK_EMBEDDED
    (void)input; (void)output; return STOCK_SPAWN_NONE;
#else
    int sockets[2]; struct rlimit files; struct sigaction child;
    if(sigaction(SIGCHLD,NULL,&child) || child.sa_handler!=SIG_DFL || (child.sa_flags&SA_NOCLDWAIT)) return STOCK_SPAWN_FAILED;
    if(getrlimit(RLIMIT_NOFILE,&files) || files.rlim_cur>1048576) return STOCK_SPAWN_FAILED;
    int image=stock_helper_prepare();
    if(image<0) return STOCK_SPAWN_UNPREPARED;
    if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)){ close(image); return STOCK_SPAWN_FAILED; }
    int ctl=fcntl(sockets[1],F_DUPFD_CLOEXEC,10), media=fcntl(input,F_DUPFD_CLOEXEC,10), exe=fcntl(image,F_DUPFD_CLOEXEC,10);
    close(image);
    if(ctl<0 || media<0 || exe<0){
        if(ctl>=0) close(ctl);
        if(media>=0) close(media);
        if(exe>=0) close(exe);
        close(sockets[0]); close(sockets[1]); return STOCK_SPAWN_FAILED;
    }
    pid_t parent=getpid(), pid=fork();
    if(!pid){
        /* Only async-signal-safe operations between fork and exec. */
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || dup2(ctl,3)!=3 || dup2(media,4)!=4 ||
           dup2(exe,5)!=5 || fcntl(5,F_SETFD,FD_CLOEXEC)) stock_process_unavailable(ctl);
        close(0); close(1); close(2);
#ifdef SYS_close_range
        if(syscall(SYS_close_range,6,~0u,0))
#endif
            for(unsigned fd=6;fd<files.rlim_cur;fd++) close((int)fd);
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK,&none,NULL);
        char *const args[]={(char *)"diskos-audiodec",NULL};
        char *const env[]={"PATH=/usr/bin:/bin",NULL};
        /* A writer descriptor another thread's fork inherited for an instant
         * makes exec refuse the copy (ETXTBSY) until that child execs. */
        for(int tries=0;tries<50;tries++){
            fexecve(5,args,env);
            if(errno!=ETXTBSY) break;
            const long delay[2]={0,2000000};
            (void)syscall(SYS_nanosleep,delay,NULL);   /* MIPS o32 musl (time64) has no SYS_clock_nanosleep; kernel 4.4 lacks the time64 calls */
        }
        stock_process_unavailable(3);
    }
    close(ctl); close(media); close(exe); close(sockets[1]);
    if(pid<0){ close(sockets[0]); return STOCK_SPAWN_FAILED; }
    *output=sockets[0]; return pid;
#endif
}
#endif
