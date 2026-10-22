/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <mqueue.h>
#include <stdint.h>
#include <poll.h>
#include <sys/uio.h>

#define DEFAULT_PORT       8080
#define DEFAULT_WEB_DIR    "/usr/data/web"
#define FALLBACK_WEB_DIR   "./web"
#define DEFAULT_CONF_PATH  "/usr/data/diskos.conf"
#define SD_ROOT            "/tmp/sdcard"
#define FIIO_IPC_HOST      "127.0.0.1"
#define FIIO_IPC_PORT      12100
#define BUFFER_SIZE        8192
#define MAX_SESSIONS       32

static int g_port = DEFAULT_PORT;
static char g_web_dir[256] = DEFAULT_WEB_DIR;
static char g_conf_path[256] = DEFAULT_CONF_PATH;
static char g_password[64] = "1234";
static pthread_mutex_t g_auth_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char token[36];
    time_t expires;
} session_t;

static session_t g_sessions[MAX_SESSIONS];

/* Load password from /usr/data/diskos.conf */
static void load_config_password(void) {
    FILE *f = fopen(g_conf_path, "r");
    if(!f) return;
    char line[256];
    while(fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if(nl) *nl = 0;
        if(strncmp(line, "web_pw=", 7) == 0) {
            pthread_mutex_lock(&g_auth_mu);
            strncpy(g_password, line + 7, sizeof(g_password) - 1);
            g_password[sizeof(g_password) - 1] = 0;
            pthread_mutex_unlock(&g_auth_mu);
            break;
        }
    }
    fclose(f);
}

/* Session token generator */
static void gen_token(char *out, size_t len) {
    static const char hex[] = "0123456789abcdef";
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned char b[16];
    if(fd >= 0) {
        if(read(fd, b, sizeof(b)) != sizeof(b)) {
            for(int i = 0; i < 16; i++) b[i] = (unsigned char)rand();
        }
        close(fd);
    } else {
        for(int i = 0; i < 16; i++) b[i] = (unsigned char)rand();
    }
    for(int i = 0; i < 16 && (size_t)(i * 2 + 1) < len; i++) {
        out[i * 2] = hex[(b[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[b[i] & 0x0f];
    }
    out[32] = 0;
}

static int check_auth(const char *token) {
    pthread_mutex_lock(&g_auth_mu);
    /* If password is empty, authentication is disabled */
    if(g_password[0] == 0) {
        pthread_mutex_unlock(&g_auth_mu);
        return 1;
    }
    if(!token || token[0] == 0) {
        pthread_mutex_unlock(&g_auth_mu);
        return 0;
    }
    time_t now = time(NULL);
    for(int i = 0; i < MAX_SESSIONS; i++) {
        if(g_sessions[i].token[0] && strcmp(g_sessions[i].token, token) == 0) {
            if(g_sessions[i].expires > now) {
                pthread_mutex_unlock(&g_auth_mu);
                return 1;
            } else {
                g_sessions[i].token[0] = 0; /* Expired */
            }
        }
    }
    pthread_mutex_unlock(&g_auth_mu);
    return 0;
}

static int add_session(char *token_out, size_t len) {
    pthread_mutex_lock(&g_auth_mu);
    gen_token(token_out, len);
    time_t now = time(NULL);
    int slot = 0;
    time_t oldest = g_sessions[0].expires;
    for(int i = 0; i < MAX_SESSIONS; i++) {
        if(g_sessions[i].token[0] == 0 || g_sessions[i].expires <= now) {
            slot = i;
            break;
        }
        if(g_sessions[i].expires < oldest) {
            oldest = g_sessions[i].expires;
            slot = i;
        }
    }
    snprintf(g_sessions[slot].token, sizeof(g_sessions[slot].token), "%s", token_out);
    g_sessions[slot].expires = now + 86400; /* 24 hours */
    pthread_mutex_unlock(&g_auth_mu);
    return 0;
}

/* URL decoding helper */
static void url_decode(char *dst, const char *src, size_t max) {
    size_t d = 0;
    while(*src && d + 1 < max) {
        if(*src == '%' && src[1] && src[2]) {
            int val = 0;
            if(sscanf(src + 1, "%2x", &val) == 1) {
                dst[d++] = (char)val;
                src += 3;
                continue;
            }
        } else if(*src == '+') {
            dst[d++] = ' ';
            src++;
            continue;
        }
        dst[d++] = *src++;
    }
    dst[d] = 0;
}

/* Send command frame directly to mq_player via POSIX message queue /player */
static int send_player_cmd(const char *cmd) {
    mqd_t q = mq_open("/player", O_WRONLY | O_NONBLOCK);
    if(q == (mqd_t)-1) return -1;
    size_t len = strlen(cmd);
    int res = -1;
    for(int i = 0; i < 5; i++) {
        if(mq_send(q, cmd, len, 0) == 0) {
            res = 0;
            break;
        }
        if(errno != EAGAIN) break;
        usleep(3000);
    }
    mq_close(q);
    return res;
}

/* Extract JSON string value for a key */
static void json_get_string(const char *json, const char *key, char *out, size_t max) {
    out[0] = 0;
    if(!json || !key) return;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    char *p = strstr(json, pat);
    if(!p) return;
    p += strlen(pat);
    while(*p == ' ' || *p == '\t') p++;
    if(*p != '"') return;
    p++; /* skip opening quote */
    size_t d = 0;
    while(*p && *p != '"' && d + 1 < max) {
        if(*p == '\\' && *(p + 1)) {
            p++;
            if(*p == '"') out[d++] = '"';
            else if(*p == '\\') out[d++] = '\\';
            else if(*p == '/') out[d++] = '/';
            else out[d++] = *p;
            p++;
            continue;
        }
        out[d++] = *p++;
    }
    out[d] = 0;
}

static void escape_json(char *dst, const char *src, size_t max) {
    if(!src) { if(max > 0) dst[0] = 0; return; }
    size_t d = 0;
    while(*src && d + 2 < max) {
        if(*src == '"' || *src == '\\') {
            dst[d++] = '\\';
            dst[d++] = *src++;
        } else if((unsigned char)*src < 32) {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = 0;
}

static void get_config_value(const char *key, char *out, size_t max, const char *def) {
    strncpy(out, def ? def : "", max - 1);
    out[max - 1] = 0;
    FILE *f = fopen(g_conf_path, "r");
    if(!f) return;
    char line[512];
    size_t klen = strlen(key);
    while(fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if(nl) *nl = 0;
        if(strncmp(line, key, klen) == 0 && line[klen] == '=') {
            strncpy(out, line + klen + 1, max - 1);
            out[max - 1] = 0;
            break;
        }
    }
    fclose(f);
}

static int update_config_value(const char *key, const char *val) {
    if(!key || !key[0]) return -1;
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_conf_path);

    FILE *fin = fopen(g_conf_path, "r");
    FILE *fout = fopen(tmp_path, "w");
    if(!fout) {
        if(fin) fclose(fin);
        return -1;
    }

    int found = 0;
    size_t klen = strlen(key);
    if(fin) {
        char line[512];
        while(fgets(line, sizeof(line), fin)) {
            char *nl = strpbrk(line, "\r\n");
            if(nl) *nl = 0;
            if(strncmp(line, key, klen) == 0 && line[klen] == '=') {
                fprintf(fout, "%s=%s\n", key, val ? val : "");
                found = 1;
            } else if(line[0]) {
                fprintf(fout, "%s\n", line);
            }
        }
        fclose(fin);
    }
    if(!found) {
        fprintf(fout, "%s=%s\n", key, val ? val : "");
    }
    fclose(fout);
    return rename(tmp_path, g_conf_path);
}

static int is_audio_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if(!dot) return 0;
    if(!strcasecmp(dot, ".flac") || !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav") ||
       !strcasecmp(dot, ".dsf") || !strcasecmp(dot, ".dff") || !strcasecmp(dot, ".m4a") ||
       !strcasecmp(dot, ".aac") || !strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".opus") ||
       !strcasecmp(dot, ".ape") || !strcasecmp(dot, ".wma") || !strcasecmp(dot, ".alac")) {
        return 1;
    }
    return 0;
}

static int play_song_file(const char *rel_or_abs) {
    char abs_target[1024];
    if(strncmp(rel_or_abs, SD_ROOT, strlen(SD_ROOT)) == 0) {
        strncpy(abs_target, rel_or_abs, sizeof(abs_target) - 1);
        abs_target[sizeof(abs_target) - 1] = 0;
    } else {
        snprintf(abs_target, sizeof(abs_target), "%s/%s", SD_ROOT, rel_or_abs ? rel_or_abs : "");
    }

    struct stat st;
    if(stat(abs_target, &st) != 0) {
        return -1;
    }

    char parent_dir[1024];
    strncpy(parent_dir, abs_target, sizeof(parent_dir) - 1);
    parent_dir[sizeof(parent_dir) - 1] = 0;
    char *last_slash = strrchr(parent_dir, '/');
    char target_fname[1024] = "";
    if(last_slash) {
        snprintf(target_fname, sizeof(target_fname), "%s", last_slash + 1);
        if(last_slash == parent_dir) *(last_slash + 1) = 0;
        else *last_slash = 0;
    } else {
        snprintf(parent_dir, sizeof(parent_dir), "%s", SD_ROOT);
        snprintf(target_fname, sizeof(target_fname), "%s", abs_target);
    }

    FILE *sql_f = fopen("/tmp/diskos_play.sql", "w");
    if(!sql_f) return -1;

    fprintf(sql_f, "PRAGMA busy_timeout=3000;\n");
    fprintf(sql_f, "BEGIN IMMEDIATE;\n");
    fprintf(sql_f, "INSERT OR IGNORE INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME,M3U_PATH) VALUES (-1000,'diskos-web','');\n");
    fprintf(sql_f, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=-1000;\n");

    DIR *d = opendir(parent_dir);
    int target_idx = 0;

    if(d) {
        struct dirent *de;
        char *track_names[256];
        int track_cnt = 0;
        while((de = readdir(d)) != NULL && track_cnt < 256) {
            if(de->d_name[0] == '.') continue;
            if(is_audio_ext(de->d_name)) {
                track_names[track_cnt++] = strdup(de->d_name);
            }
        }
        closedir(d);

        for(int i = 0; i < track_cnt - 1; i++) {
            for(int j = i + 1; j < track_cnt; j++) {
                if(strcasecmp(track_names[i], track_names[j]) > 0) {
                    char *tmp = track_names[i];
                    track_names[i] = track_names[j];
                    track_names[j] = tmp;
                }
            }
        }

        for(int i = 0; i < track_cnt; i++) {
            if(strcmp(track_names[i], target_fname) == 0) {
                target_idx = i;
            }
            char song_path[1300];
            snprintf(song_path, sizeof(song_path), "%s/%s", parent_dir, track_names[i]);

            char esc_path[2048];
            int ei = 0;
            for(int k = 0; song_path[k] && ei + 2 < (int)sizeof(esc_path); k++) {
                if(song_path[k] == '\'') { esc_path[ei++] = '\''; esc_path[ei++] = '\''; }
                else esc_path[ei++] = song_path[k];
            }
            esc_path[ei] = 0;

            char esc_name[512];
            ei = 0;
            for(int k = 0; track_names[i][k] && ei + 2 < (int)sizeof(esc_name); k++) {
                if(track_names[i][k] == '\'') { esc_name[ei++] = '\''; esc_name[ei++] = '\''; }
                else esc_name[ei++] = track_names[i][k];
            }
            esc_name[ei] = 0;

            fprintf(sql_f,
                "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST) "
                "SELECT -1000,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),ALBUM_ARTIST "
                "FROM SONG WHERE PATH='%s';\n"
                "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,DURATION) "
                "SELECT -1000,'%s','%s','%s',86400000 WHERE NOT EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=-1000 AND PATH='%s');\n",
                esc_path, esc_path, esc_name, esc_name, esc_path);

            free(track_names[i]);
        }
    } else {
        char esc_path[2048];
        int ei = 0;
        for(int k = 0; abs_target[k] && ei + 2 < (int)sizeof(esc_path); k++) {
            if(abs_target[k] == '\'') { esc_path[ei++] = '\''; esc_path[ei++] = '\''; }
            else esc_path[ei++] = abs_target[k];
        }
        esc_path[ei] = 0;

        fprintf(sql_f,
            "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST) "
            "SELECT -1000,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),ALBUM_ARTIST "
            "FROM SONG WHERE PATH='%s';\n"
            "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,DURATION) "
            "SELECT -1000,'%s','%s','%s',86400000 WHERE NOT EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=-1000 AND PATH='%s');\n",
            esc_path, esc_path, target_fname, target_fname, esc_path);
    }

    fprintf(sql_f, "COMMIT;\n");
    fclose(sql_f);

    int rc = system("sqlite3 /usr/data/fiio/db/song.db < /tmp/diskos_play.sql >/dev/null 2>&1");
    unlink("/tmp/diskos_play.sql");
    if(rc != 0) return -1;

    send_player_cmd("0657000C0008");

    char play_frame[32];
    snprintf(play_frame, sizeof(play_frame), "01000010%04X0005", target_idx & 0xFFFF);
    send_player_cmd(play_frame);

    usleep(50000);
    send_player_cmd("0201000C0000");

    return 0;
}

/* Cached system telemetry and configuration */
static int g_cached_work_mode = 0;
static time_t g_last_conf_check = 0;
static int g_cached_batt = -1;
static time_t g_last_batt_read = 0;
static time_t g_last_json_mtime = 0;
static char g_cached_clean_json[3584] = "{}";
static volatile int g_active_ws_clients = 0;

static int get_cached_work_mode(void) {
    time_t now = time(NULL);
    if(now - g_last_conf_check >= 10 || g_last_conf_check == 0) {
        char pm_s[16] = "0";
        get_config_value("work_mode", pm_s, sizeof(pm_s), "0");
        g_cached_work_mode = atoi(pm_s);
        g_last_conf_check = now;
    }
    return g_cached_work_mode;
}

static int get_cached_battery(void) {
    time_t now = time(NULL);
    if(now - g_last_batt_read >= 5 || g_last_batt_read == 0) {
        FILE *bf = fopen("/sys/class/power_supply/cw221X-bat/capacity", "r");
        if(bf) {
            if(fscanf(bf, "%d", &g_cached_batt) != 1) g_cached_batt = -1;
            fclose(bf);
        }
        g_last_batt_read = now;
    }
    return g_cached_batt;
}

/* Execute player command (shared between HTTP API and WebSocket) */
static int execute_player_action(const char *action, const char *path, long ms, int val) {
    if(strcasecmp(action, "play_file") == 0 || (strcasecmp(action, "play") == 0 && path && path[0])) {
        if(path && path[0]) {
            return play_song_file(path);
        }
    }
    if(strcasecmp(action, "play_idx") == 0 && val >= 0) {
        send_player_cmd("0657000C0008");
        char play_frame[32];
        snprintf(play_frame, sizeof(play_frame), "01000010%04X0005", (unsigned int)(val & 0xFFFF));
        send_player_cmd(play_frame);
        usleep(50000);
        return send_player_cmd("0201000C0000");
    }
    if(strcasecmp(action, "play_mode") == 0 && val >= 0 && val <= 4) {
        char m[32];
        snprintf(m, sizeof(m), "0102000C%04X", (unsigned int)(val & 0xFFFF));
        char pm_s[16]; snprintf(pm_s, sizeof(pm_s), "%d", val);
        update_config_value("work_mode", pm_s);
        g_cached_work_mode = val;
        g_last_conf_check = time(NULL);
        return send_player_cmd(m);
    }
    if(strcasecmp(action, "clear_queue") == 0) {
        system("sqlite3 /usr/data/fiio/db/song.db \"PRAGMA busy_timeout=2000; DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=-1000;\" >/dev/null 2>&1");
        return 0;
    }
    char ipc_frame[64] = "";
    if(strcasecmp(action, "toggle") == 0 || strcasecmp(action, "play") == 0 || strcasecmp(action, "pause") == 0) {
        strcpy(ipc_frame, "0201000C0000");
    } else if(strcasecmp(action, "next") == 0) {
        strcpy(ipc_frame, "0201000C0001");
    } else if(strcasecmp(action, "prev") == 0) {
        strcpy(ipc_frame, "0201000C0002");
    } else if(strcasecmp(action, "seek") == 0 && ms >= 0) {
        snprintf(ipc_frame, sizeof(ipc_frame), "01030010%08lX", (unsigned long)ms);
    } else if(strcasecmp(action, "volume") == 0 && val >= 0) {
        if(val > 120) val = 120;
        snprintf(ipc_frame, sizeof(ipc_frame), "0715000C%04X", (unsigned int)val);
    }
    if(ipc_frame[0]) {
        return send_player_cmd(ipc_frame);
    }
    return -2; /* Unknown or incomplete action */
}

/* Read current player status and battery level into JSON string */
static void get_current_player_status(char *out, size_t max) {
    struct stat st;
    if(stat("/tmp/diskos_player.json", &st) == 0) {
        if(st.st_mtime != g_last_json_mtime || g_last_json_mtime == 0) {
            FILE *f = fopen("/tmp/diskos_player.json", "r");
            if(f) {
                size_t r = fread(g_cached_clean_json, 1, sizeof(g_cached_clean_json) - 1, f);
                g_cached_clean_json[r] = 0;
                fclose(f);
                char *nl = strpbrk(g_cached_clean_json, "\r\n");
                if(nl) *nl = 0;
                g_last_json_mtime = st.st_mtime;
            }
        }
    }

    int batt = get_cached_battery();
    int play_mode = get_cached_work_mode();

    size_t len = strlen(g_cached_clean_json);
    if(len > 1 && g_cached_clean_json[0] == '{' && g_cached_clean_json[len - 1] == '}') {
        char temp_copy[3584];
        snprintf(temp_copy, sizeof(temp_copy), "%s", g_cached_clean_json);
        temp_copy[len - 1] = 0;
        snprintf(out, max, "%s,\"battery\":%d,\"play_mode\":%d}\n", temp_copy, batt, play_mode);
    } else {
        snprintf(out, max, "{\"battery\":%d,\"play_mode\":%d}\n", batt, play_mode);
    }
}

/* ========================================================================= */
/* RFC 6455 WebSocket Implementation                                        */
/* ========================================================================= */

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} sha1_ctx_t;

#define SHA1_ROL32(val, bits) (((val) << (bits)) | ((val) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];
    for(int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for(int i = 16; i < 80; i++) {
        w[i] = SHA1_ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for(int i = 0; i < 80; i++) {
        uint32_t f, k;
        if(i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if(i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if(i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = SHA1_ROL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = SHA1_ROL32(b, 30);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = ctx->count[1] = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;
    size_t j = (ctx->count[0] >> 3) & 63;
    if((ctx->count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);
    if((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        sha1_transform(ctx->state, ctx->buffer);
        for(; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &data[i]);
        }
        j = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for(int i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    uint8_t c = 0x80;
    sha1_update(ctx, &c, 1);
    while((ctx->count[0] & 504) != 448) {
        c = 0x00;
        sha1_update(ctx, &c, 1);
    }
    sha1_update(ctx, finalcount, 8);
    for(int i = 0; i < 20; i++) {
        digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

static void base64_encode(const uint8_t *src, size_t len, char *out, size_t out_max) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while(i < len && j + 4 < out_max) {
        size_t rem = len - i;
        uint32_t octet_a = src[i++];
        uint32_t octet_b = (rem > 1) ? src[i++] : 0;
        uint32_t octet_c = (rem > 2) ? src[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = b64[(triple >> 18) & 0x3F];
        out[j++] = b64[(triple >> 12) & 0x3F];
        out[j++] = (rem > 1) ? b64[(triple >> 6) & 0x3F] : '=';
        out[j++] = (rem > 2) ? b64[triple & 0x3F] : '=';
    }
    out[j] = '\0';
}

static void ws_make_accept_key(const char *client_key, char *out_accept, size_t max_out) {
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", client_key, guid);

    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t *)concat, strlen(concat));
    uint8_t digest[20];
    sha1_final(&ctx, digest);

    base64_encode(digest, 20, out_accept, max_out);
}

static int ws_send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t len) {
    uint8_t header[10];
    size_t hlen = 0;

    header[0] = 0x80 | (opcode & 0x0F); /* FIN = 1 */
    if(len <= 125) {
        header[1] = (uint8_t)len;
        hlen = 2;
    } else if(len <= 65535) {
        header[1] = 126;
        header[2] = (uint8_t)((len >> 8) & 0xFF);
        header[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        header[1] = 127;
        for(int i = 7; i >= 0; i--) {
            header[2 + (7 - i)] = (uint8_t)((len >> (i * 8)) & 0xFF);
        }
        hlen = 10;
    }

    struct iovec iov[2];
    iov[0].iov_base = header;
    iov[0].iov_len = hlen;
    int iovcnt = 1;

    if(len > 0 && payload != NULL) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = len;
        iovcnt = 2;
    }

    ssize_t exp = (ssize_t)(hlen + len);
    if(writev(fd, iov, iovcnt) != exp) {
        return -1;
    }
    return 0;
}

static int ws_send_text(int fd, const char *text) {
    if(!text) return 0;
    return ws_send_frame(fd, 0x1, (const uint8_t *)text, strlen(text));
}

static int ws_read_exact(int fd, uint8_t *buf, size_t len) {
    size_t total = 0;
    while(total < len) {
        ssize_t n = read(fd, buf + total, len - total);
        if(n <= 0) {
            if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                int pr = poll(&pfd, 1, 1000);
                if(pr > 0 && (pfd.revents & POLLIN)) continue;
            }
            return -1;
        }
        total += n;
    }
    return 0;
}

static int ws_read_frame(int fd, uint8_t *out_opcode, char *out_payload, size_t max_len, size_t *out_len) {
    uint8_t hdr[2];
    if(ws_read_exact(fd, hdr, 2) != 0) return -1;

    *out_opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    if(payload_len == 126) {
        uint8_t ext[2];
        if(ws_read_exact(fd, ext, 2) != 0) return -1;
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if(payload_len == 127) {
        uint8_t ext[8];
        if(ws_read_exact(fd, ext, 8) != 0) return -1;
        payload_len = 0;
        for(int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
    }

    if(payload_len >= max_len) {
        return -2; /* Frame too large */
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if(masked) {
        if(ws_read_exact(fd, mask, 4) != 0) return -1;
    }

    if(payload_len > 0) {
        if(ws_read_exact(fd, (uint8_t *)out_payload, payload_len) != 0) return -1;
        if(masked) {
            for(size_t i = 0; i < payload_len; i++) {
                out_payload[i] ^= mask[i % 4];
            }
        }
    }
    out_payload[payload_len] = '\0';
    *out_len = (size_t)payload_len;
    return 0;
}

static void handle_websocket(int fd) {
    char last_status[4096] = "";
    char cur_status[4096] = "";
    char in_payload[2048];
    uint8_t opcode = 0;
    size_t in_len = 0;
    int tick_count = 0;

    if(__sync_fetch_and_add(&g_active_ws_clients, 1) == 0) {
        int afd = creat("/tmp/diskos_ws_active", 0644);
        if(afd >= 0) close(afd);
    }

    /* Send initial status immediately upon connection */
    get_current_player_status(cur_status, sizeof(cur_status));
    ws_send_text(fd, cur_status);
    snprintf(last_status, sizeof(last_status), "%s", cur_status);

    while(1) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN | POLLERR | POLLHUP;
        pfd.revents = 0;

        /* Poll with 1000ms timeout (immediate on POLLIN) */
        int ret = poll(&pfd, 1, 1000);
        if(ret < 0) {
            if(errno == EINTR) continue;
            break;
        }

        if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break; /* Client disconnected */
        }

        if(pfd.revents & POLLIN) {
            int fr = ws_read_frame(fd, &opcode, in_payload, sizeof(in_payload), &in_len);
            if(fr != 0) {
                break; /* Read error or client closed */
            }

            if(opcode == 0x8) {
                /* Close frame */
                ws_send_frame(fd, 0x8, NULL, 0);
                break;
            } else if(opcode == 0x9) {
                /* Ping frame -> reply with Pong */
                ws_send_frame(fd, 0xA, (const uint8_t *)in_payload, in_len);
            } else if(opcode == 0x1) {
                /* Text frame (client command) */
                char action[32] = "";
                char path_arg[512] = "";
                long ms = -1;
                int val = -1;

                json_get_string(in_payload, "action", action, sizeof(action));
                json_get_string(in_payload, "path", path_arg, sizeof(path_arg));

                char *m = strstr(in_payload, "\"ms\":");
                if(m) ms = atol(m + 5);
                char *v = strstr(in_payload, "\"val\":");
                if(!v) v = strstr(in_payload, "\"value\":");
                if(!v) v = strstr(in_payload, "\"idx\":");
                if(!v) v = strstr(in_payload, "\"index\":");
                if(!v) v = strstr(in_payload, "\"mode\":");
                if(v) {
                    while(*v && (*v < '0' || *v > '9') && *v != '-') v++;
                    val = atoi(v);
                }

                if(action[0]) {
                    execute_player_action(action, path_arg, ms, val);
                }

                /* After action, wait a brief moment for player state to update, then push */
                usleep(80000); /* 80ms */
                get_current_player_status(cur_status, sizeof(cur_status));
                if(ws_send_text(fd, cur_status) != 0) break;
                snprintf(last_status, sizeof(last_status), "%s", cur_status);
                tick_count = 0;
                continue;
            }
        }

        /* Periodic check (every 1000ms tick) */
        tick_count++;
        get_current_player_status(cur_status, sizeof(cur_status));

        /* If status changed OR 5 seconds elapsed without an update (heartbeat = 5 ticks) */
        if(strcmp(cur_status, last_status) != 0 || tick_count >= 5) {
            if(ws_send_text(fd, cur_status) != 0) {
                break; /* Failed to send -> client disconnected */
            }
            snprintf(last_status, sizeof(last_status), "%s", cur_status);
            tick_count = 0;
        }
    }

    if(__sync_sub_and_fetch(&g_active_ws_clients, 1) == 0) {
        unlink("/tmp/diskos_ws_active");
    }
}

/* Send HTTP Response Header */
static void send_response_headers(int fd, int code, const char *status, const char *type, long content_length) {
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: diskOS-Webd\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n",
        code, status);
    if(type) {
        len += snprintf(header + len, sizeof(header) - len, "Content-Type: %s\r\n", type);
    }
    if(content_length >= 0) {
        len += snprintf(header + len, sizeof(header) - len, "Content-Length: %ld\r\n", content_length);
    }
    len += snprintf(header + len, sizeof(header) - len, "\r\n");
    write(fd, header, len);
}

static void send_static_response_headers(int fd, int code, const char *status, const char *type, long content_length, const char *cache_control) {
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: diskOS-Webd\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n",
        code, status);
    if(type) {
        len += snprintf(header + len, sizeof(header) - len, "Content-Type: %s\r\n", type);
    }
    if(cache_control) {
        len += snprintf(header + len, sizeof(header) - len, "Cache-Control: %s\r\n", cache_control);
    }
    if(content_length >= 0) {
        len += snprintf(header + len, sizeof(header) - len, "Content-Length: %ld\r\n", content_length);
    }
    len += snprintf(header + len, sizeof(header) - len, "\r\n");
    write(fd, header, len);
}

static void send_json_error(int fd, int code, const char *status, const char *msg) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}\n", msg);
    send_response_headers(fd, code, status, "application/json", strlen(body));
    write(fd, body, strlen(body));
}

static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if(!dot) return "application/octet-stream";
    if(!strcasecmp(dot, ".html")) return "text/html; charset=utf-8";
    if(!strcasecmp(dot, ".css")) return "text/css; charset=utf-8";
    if(!strcasecmp(dot, ".js")) return "application/javascript; charset=utf-8";
    if(!strcasecmp(dot, ".json")) return "application/json";
    if(!strcasecmp(dot, ".png")) return "image/png";
    if(!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if(!strcasecmp(dot, ".svg")) return "image/svg+xml";
    if(!strcasecmp(dot, ".flac")) return "audio/flac";
    if(!strcasecmp(dot, ".mp3")) return "audio/mpeg";
    if(!strcasecmp(dot, ".wav")) return "audio/wav";
    if(!strcasecmp(dot, ".dsf") || !strcasecmp(dot, ".dff")) return "audio/x-dsd";
    if(!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".aac")) return "audio/mp4";
    if(!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".opus")) return "audio/ogg";
    return "application/octet-stream";
}

/* Safe path check for /tmp/sdcard */
static int is_safe_sd_path(const char *rel_path, char *resolved_out, size_t max) {
    char combined[1024];
    snprintf(combined, sizeof(combined), "%s/%s", SD_ROOT, rel_path ? rel_path : "");
    if(!realpath(combined, resolved_out)) {
        /* If file doesn't exist yet (for upload), resolve parent dir */
        char parent[1024];
        strncpy(parent, combined, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = 0;
        char *slash = strrchr(parent, '/');
        if(slash && slash != parent) {
            *slash = 0;
            char resolved_parent[1024];
            if(realpath(parent, resolved_parent)) {
                if(strncmp(resolved_parent, SD_ROOT, strlen(SD_ROOT)) == 0) {
                    strncpy(resolved_out, combined, max - 1);
                    resolved_out[max - 1] = 0;
                    return 1;
                }
            }
        }
        return 0;
    }
    if(strncmp(resolved_out, SD_ROOT, strlen(SD_ROOT)) == 0) {
        return 1;
    }
    return 0;
}

/* Client Connection Handler */
static void *handle_client(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[4096];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if(n <= 0) { close(fd); return NULL; }
    req[n] = 0;

    char method[16] = "", uri[1024] = "", proto[16] = "";
    sscanf(req, "%15s %1023s %15s", method, uri, proto);

    if(strcasecmp(method, "OPTIONS") == 0) {
        send_response_headers(fd, 204, "No Content", NULL, 0);
        close(fd);
        return NULL;
    }

    /* Extract Query Parameters and Token */
    char path_part[512] = "", query_part[512] = "";
    char *qmark = strchr(uri, '?');
    if(qmark) {
        size_t plen = (size_t)(qmark - uri);
        if(plen >= sizeof(path_part)) plen = sizeof(path_part) - 1;
        strncpy(path_part, uri, plen);
        path_part[plen] = 0;
        snprintf(query_part, sizeof(query_part), "%s", qmark + 1);
    } else {
        snprintf(path_part, sizeof(path_part), "%s", uri);
    }

    /* Parse Auth Token from Header or Query Parameter */
    char token[64] = "";
    char *auth_hdr = strcasestr(req, "Authorization: Bearer ");
    if(auth_hdr) {
        auth_hdr += 22;
        char *end = strpbrk(auth_hdr, "\r\n ");
        size_t tlen = end ? (size_t)(end - auth_hdr) : strlen(auth_hdr);
        if(tlen < sizeof(token)) {
            strncpy(token, auth_hdr, tlen);
            token[tlen] = 0;
        }
    } else if(query_part[0]) {
        char *tok_q = strstr(query_part, "token=");
        if(tok_q) {
            tok_q += 6;
            char *end = strchr(tok_q, '&');
            size_t tlen = end ? (size_t)(end - tok_q) : strlen(tok_q);
            if(tlen < sizeof(token)) {
                strncpy(token, tok_q, tlen);
                token[tlen] = 0;
            }
        }
    }

    /* Route: POST /api/auth */
    if(strcasecmp(method, "POST") == 0 && strcmp(path_part, "/api/auth") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if(body) body += 4;
        char input_pw[64] = "";
        if(body) {
            char *p = strstr(body, "\"password\":");
            if(p) {
                p += 11;
                while(*p == ' ' || *p == '"') p++;
                char *end = strpbrk(p, "\"}\r\n");
                if(end) {
                    size_t pwlen = (size_t)(end - p);
                    if(pwlen < sizeof(input_pw)) {
                        strncpy(input_pw, p, pwlen);
                        input_pw[pwlen] = 0;
                    }
                }
            }
        }

        load_config_password();
        pthread_mutex_lock(&g_auth_mu);
        int valid = (g_password[0] == 0 || strcmp(input_pw, g_password) == 0);
        pthread_mutex_unlock(&g_auth_mu);

        if(valid) {
            char new_tok[36];
            add_session(new_tok, sizeof(new_tok));
            char resp_body[128];
            snprintf(resp_body, sizeof(resp_body), "{\"ok\":true,\"token\":\"%s\"}\n", new_tok);
            send_response_headers(fd, 200, "OK", "application/json", strlen(resp_body));
            write(fd, resp_body, strlen(resp_body));
        } else {
            send_json_error(fd, 401, "Unauthorized", "Incorrect PIN or password");
        }
        close(fd);
        return NULL;
    }

    /* Favicon handler */
    if(strcmp(path_part, "/favicon.ico") == 0) {
        send_response_headers(fd, 204, "No Content", NULL, 0);
        close(fd);
        return NULL;
    }

    /* Route: WebSocket /ws or /api/ws */
    char *hdr_upgrade = strcasestr(req, "Upgrade: websocket");
    char *hdr_sec_key = strcasestr(req, "Sec-WebSocket-Key:");
    if(hdr_upgrade && hdr_sec_key && (strcmp(path_part, "/ws") == 0 || strcmp(path_part, "/api/ws") == 0)) {
        if(!check_auth(token)) {
            send_json_error(fd, 401, "Unauthorized", "Authentication required");
            close(fd);
            return NULL;
        }

        char sec_key[128] = "";
        char *k = hdr_sec_key + 18;
        while(*k == ' ' || *k == '\t') k++;
        char *end = strpbrk(k, "\r\n");
        if(end) {
            size_t klen = (size_t)(end - k);
            if(klen < sizeof(sec_key)) {
                strncpy(sec_key, k, klen);
                sec_key[klen] = 0;
            }
        }

        if(sec_key[0] == 0) {
            send_json_error(fd, 400, "Bad Request", "Missing Sec-WebSocket-Key");
            close(fd);
            return NULL;
        }

        char accept_key[64];
        ws_make_accept_key(sec_key, accept_key, sizeof(accept_key));

        char resp[512];
        int rlen = snprintf(resp, sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "\r\n", accept_key);
        if(write(fd, resp, rlen) <= 0) {
            close(fd);
            return NULL;
        }

        handle_websocket(fd);
        close(fd);
        return NULL;
    }

    /* All other /api/ routes require valid authentication (except album art for <img> tags) */
    if(strncmp(path_part, "/api/", 5) == 0 && strcmp(path_part, "/api/player/art") != 0) {
        if(!check_auth(token)) {
            send_json_error(fd, 401, "Unauthorized", "Authentication required");
            close(fd);
            return NULL;
        }
    }

    /* Route: GET /api/player/status */
    if(strcasecmp(method, "GET") == 0 && strcmp(path_part, "/api/player/status") == 0) {
        char final_resp[4096];
        get_current_player_status(final_resp, sizeof(final_resp));
        send_response_headers(fd, 200, "OK", "application/json", strlen(final_resp));
        write(fd, final_resp, strlen(final_resp));
        close(fd);
        return NULL;
    }

    /* Route: POST /api/player/cmd */
    if(strcasecmp(method, "POST") == 0 && strcmp(path_part, "/api/player/cmd") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if(body) body += 4;
        char action[32] = "";
        char path_arg[512] = "";
        long ms = -1;
        int val = -1;
        if(body) {
            json_get_string(body, "action", action, sizeof(action));
            json_get_string(body, "path", path_arg, sizeof(path_arg));

            char *m = strstr(body, "\"ms\":");
            if(m) ms = atol(m + 5);
            char *v = strstr(body, "\"val\":");
            if(!v) v = strstr(body, "\"value\":");
            if(!v) v = strstr(body, "\"idx\":");
            if(!v) v = strstr(body, "\"index\":");
            if(!v) v = strstr(body, "\"mode\":");
            if(v) {
                while(*v && (*v < '0' || *v > '9') && *v != '-') v++;
                val = atoi(v);
            }
        }

        if(action[0]) {
            int ok = execute_player_action(action, path_arg, ms, val);
            if(ok == 0) {
                const char *ok_body = "{\"ok\":true}\n";
                send_response_headers(fd, 200, "OK", "application/json", strlen(ok_body));
                write(fd, ok_body, strlen(ok_body));
            } else if(ok == -2) {
                send_json_error(fd, 400, "Bad Request", "Unknown action");
            } else {
                send_json_error(fd, 500, "Internal Server Error", "Could not queue command to player");
            }
        } else {
            send_json_error(fd, 400, "Bad Request", "Unknown or incomplete action");
        }
        close(fd);
        return NULL;
    }

    /* Route: GET /api/player/queue */
    if(strcasecmp(method, "GET") == 0 && strcmp(path_part, "/api/player/queue") == 0) {
        FILE *qp = popen("sqlite3 -separator '|' /usr/data/fiio/db/song.db \"PRAGMA busy_timeout=2000; SELECT TITLE, ARTIST, ALBUM, PATH, DURATION FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID = -1000 ORDER BY ROWID ASC;\"", "r");
        if(!qp) {
            send_json_error(fd, 500, "Internal Server Error", "Could not query queue");
            close(fd);
            return NULL;
        }

        char *qbuf = malloc(65536);
        if(!qbuf) {
            pclose(qp);
            send_json_error(fd, 500, "Internal Server Error", "Out of memory");
            close(fd);
            return NULL;
        }

        size_t qlen = 0;
        qlen += snprintf(qbuf + qlen, 65536 - qlen, "[");
        char qline[2048];
        int qidx = 0;
        while(fgets(qline, sizeof(qline), qp) && qlen + 1024 < 65536) {
            char *nl = strpbrk(qline, "\r\n");
            if(nl) *nl = 0;
            if(!qline[0]) continue;

            char *p_title = qline;
            char *p_artist = strchr(p_title, '|');
            if(!p_artist) continue;
            *p_artist++ = 0;
            char *p_album = strchr(p_artist, '|');
            if(!p_album) continue;
            *p_album++ = 0;
            char *p_path = strchr(p_album, '|');
            if(!p_path) continue;
            *p_path++ = 0;
            char *p_dur = strchr(p_path, '|');
            if(!p_dur) continue;
            *p_dur++ = 0;
            long dur_ms = atol(p_dur);

            char esc_t[512], esc_art[384], esc_alb[384], esc_p[1024];
            escape_json(esc_t, p_title, sizeof(esc_t));
            escape_json(esc_art, p_artist, sizeof(esc_art));
            escape_json(esc_alb, p_album, sizeof(esc_alb));
            escape_json(esc_p, p_path, sizeof(esc_p));

            qlen += snprintf(qbuf + qlen, 65536 - qlen,
                "%s{\"idx\":%d,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"path\":\"%s\",\"duration\":%ld}",
                (qidx > 0) ? "," : "", qidx, esc_t, esc_art, esc_alb, esc_p, dur_ms);
            qidx++;
        }
        pclose(qp);
        qlen += snprintf(qbuf + qlen, 65536 - qlen, "]\n");

        send_response_headers(fd, 200, "OK", "application/json", qlen);
        write(fd, qbuf, qlen);
        free(qbuf);
        close(fd);
        return NULL;
    }

    /* Route: GET /api/files/list?path=... */
    if(strcasecmp(method, "GET") == 0 && strcmp(path_part, "/api/files/list") == 0) {
        char req_path[512] = "";
        char *p_arg = strstr(query_part, "path=");
        if(p_arg) {
            p_arg += 5;
            char *end = strchr(p_arg, '&');
            size_t plen = end ? (size_t)(end - p_arg) : strlen(p_arg);
            char enc[512] = "";
            if(plen < sizeof(enc)) {
                strncpy(enc, p_arg, plen);
                enc[plen] = 0;
                url_decode(req_path, enc, sizeof(req_path));
            }
        }

        char abs_path[1024];
        if(!is_safe_sd_path(req_path, abs_path, sizeof(abs_path))) {
            send_json_error(fd, 403, "Forbidden", "Access outside SD card forbidden");
            close(fd);
            return NULL;
        }

        DIR *d = opendir(abs_path);
        if(!d) {
            send_json_error(fd, 404, "Not Found", "Directory not found");
            close(fd);
            return NULL;
        }

        send_response_headers(fd, 200, "OK", "application/json", -1);
        write(fd, "[", 1);
        struct dirent *de;
        int first = 1;
        while((de = readdir(d))) {
            if(de->d_name[0] == '.') continue;
            char item_path[1400];
            snprintf(item_path, sizeof(item_path), "%s/%s", abs_path, de->d_name);
            struct stat st;
            if(stat(item_path, &st) != 0) continue;

            int is_dir = S_ISDIR(st.st_mode);
            char item_json[512];
            snprintf(item_json, sizeof(item_json),
                "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%lld,\"mtime\":%ld}",
                first ? "" : ",", de->d_name, is_dir ? "true" : "false",
                (long long)st.st_size, (long)st.st_mtime);
            write(fd, item_json, strlen(item_json));
            first = 0;
        }
        write(fd, "]\n", 2);
        closedir(d);
        close(fd);
        return NULL;
    }

    /* Route: GET /api/files/download?path=... */
    if(strcasecmp(method, "GET") == 0 && strcmp(path_part, "/api/files/download") == 0) {
        char req_path[512] = "";
        char *p_arg = strstr(query_part, "path=");
        if(p_arg) {
            p_arg += 5;
            char *end = strchr(p_arg, '&');
            size_t plen = end ? (size_t)(end - p_arg) : strlen(p_arg);
            char enc[512] = "";
            if(plen < sizeof(enc)) {
                strncpy(enc, p_arg, plen);
                enc[plen] = 0;
                url_decode(req_path, enc, sizeof(req_path));
            }
        }

        char abs_path[1024];
        if(!is_safe_sd_path(req_path, abs_path, sizeof(abs_path))) {
            send_json_error(fd, 403, "Forbidden", "Access outside SD card forbidden");
            close(fd);
            return NULL;
        }

        int ffd = open(abs_path, O_RDONLY);
        if(ffd < 0) {
            send_json_error(fd, 404, "Not Found", "File not found");
            close(fd);
            return NULL;
        }

        struct stat st;
        fstat(ffd, &st);
        send_response_headers(fd, 200, "OK", get_mime_type(abs_path), st.st_size);

        char buf[BUFFER_SIZE];
        ssize_t bytes;
        while((bytes = read(ffd, buf, sizeof(buf))) > 0) {
            if(write(fd, buf, bytes) <= 0) break;
        }
        close(ffd);
        close(fd);
        return NULL;
    }

    /* Route: POST /api/files/upload?dir=...&filename=... */
    if(strcasecmp(method, "POST") == 0 && strcmp(path_part, "/api/files/upload") == 0) {
        char dir_arg[512] = "", file_arg[256] = "";
        char *d_p = strstr(query_part, "dir=");
        if(d_p) {
            d_p += 4;
            char *end = strchr(d_p, '&');
            size_t len = end ? (size_t)(end - d_p) : strlen(d_p);
            char enc[512] = "";
            if(len < sizeof(enc)) {
                strncpy(enc, d_p, len); enc[len] = 0;
                url_decode(dir_arg, enc, sizeof(dir_arg));
            }
        }
        char *f_p = strstr(query_part, "filename=");
        if(f_p) {
            f_p += 9;
            char *end = strchr(f_p, '&');
            size_t len = end ? (size_t)(end - f_p) : strlen(f_p);
            char enc[256] = "";
            if(len < sizeof(enc)) {
                strncpy(enc, f_p, len); enc[len] = 0;
                url_decode(file_arg, enc, sizeof(file_arg));
            }
        }

        if(!file_arg[0]) {
            send_json_error(fd, 400, "Bad Request", "Missing filename");
            close(fd);
            return NULL;
        }

        char rel_full[768];
        snprintf(rel_full, sizeof(rel_full), "%s/%s", dir_arg, file_arg);
        char abs_path[1024];
        if(!is_safe_sd_path(rel_full, abs_path, sizeof(abs_path))) {
            send_json_error(fd, 403, "Forbidden", "Access outside SD card forbidden");
            close(fd);
            return NULL;
        }

        /* Extract Content-Length */
        long content_len = 0;
        char *cl_hdr = strcasestr(req, "Content-Length: ");
        if(cl_hdr) content_len = atol(cl_hdr + 16);

        char *body = strstr(req, "\r\n\r\n");
        if(!body) {
            send_json_error(fd, 400, "Bad Request", "Malformed HTTP request");
            close(fd);
            return NULL;
        }
        body += 4;
        size_t initial_bytes = n - (body - req);

        int out_fd = open(abs_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if(out_fd < 0) {
            send_json_error(fd, 500, "Internal Server Error", "Could not create target file");
            close(fd);
            return NULL;
        }

        if(initial_bytes > 0) {
            write(out_fd, body, initial_bytes);
        }

        long total_written = (long)initial_bytes;
        char buf[BUFFER_SIZE];
        while(total_written < content_len) {
            size_t to_read = sizeof(buf);
            if(content_len - total_written < (long)to_read) {
                to_read = content_len - total_written;
            }
            ssize_t got = read(fd, buf, to_read);
            if(got <= 0) break;
            write(out_fd, buf, got);
            total_written += got;
        }
        close(out_fd);
        sync();

        const char *ok_resp = "{\"ok\":true}\n";
        send_response_headers(fd, 200, "OK", "application/json", strlen(ok_resp));
        write(fd, ok_resp, strlen(ok_resp));
        close(fd);
        return NULL;
    }

    /* Route: DELETE /api/files/delete?path=... */
    if(strcasecmp(method, "DELETE") == 0 && strcmp(path_part, "/api/files/delete") == 0) {
        char req_path[512] = "";
        char *p_arg = strstr(query_part, "path=");
        if(p_arg) {
            p_arg += 5;
            char *end = strchr(p_arg, '&');
            size_t plen = end ? (size_t)(end - p_arg) : strlen(p_arg);
            char enc[512] = "";
            if(plen < sizeof(enc)) {
                strncpy(enc, p_arg, plen); enc[plen] = 0;
                url_decode(req_path, enc, sizeof(req_path));
            }
        }

        char abs_path[1024];
        if(!is_safe_sd_path(req_path, abs_path, sizeof(abs_path)) || strcmp(abs_path, SD_ROOT) == 0) {
            send_json_error(fd, 403, "Forbidden", "Cannot delete SD root or unsafe path");
            close(fd);
            return NULL;
        }

        struct stat st;
        if(stat(abs_path, &st) != 0) {
            send_json_error(fd, 404, "Not Found", "Item does not exist");
            close(fd);
            return NULL;
        }

        int res = S_ISDIR(st.st_mode) ? rmdir(abs_path) : unlink(abs_path);
        if(res == 0) {
            const char *ok_body = "{\"ok\":true}\n";
            send_response_headers(fd, 200, "OK", "application/json", strlen(ok_body));
            write(fd, ok_body, strlen(ok_body));
        } else {
            send_json_error(fd, 500, "Internal Server Error", "Could not delete file/folder (directory not empty?)");
        }
        close(fd);
        return NULL;
    }

    /* Route: POST /api/files/mkdir */
    if(strcasecmp(method, "POST") == 0 && strcmp(path_part, "/api/files/mkdir") == 0) {
        char req_dir[512] = "";
        char req_name[256] = "";

        char *p_dir = strstr(query_part, "dir=");
        if(p_dir) {
            p_dir += 4;
            char *end = strchr(p_dir, '&');
            size_t plen = end ? (size_t)(end - p_dir) : strlen(p_dir);
            char enc[512] = "";
            if(plen < sizeof(enc)) {
                strncpy(enc, p_dir, plen); enc[plen] = 0;
                url_decode(req_dir, enc, sizeof(req_dir));
            }
        }
        char *p_name = strstr(query_part, "name=");
        if(p_name) {
            p_name += 5;
            char *end = strchr(p_name, '&');
            size_t plen = end ? (size_t)(end - p_name) : strlen(p_name);
            char enc[256] = "";
            if(plen < sizeof(enc)) {
                strncpy(enc, p_name, plen); enc[plen] = 0;
                url_decode(req_name, enc, sizeof(req_name));
            }
        }

        char *body = strstr(req, "\r\n\r\n");
        if(body) {
            body += 4;
            if(!req_dir[0]) json_get_string(body, "dir", req_dir, sizeof(req_dir));
            if(!req_name[0]) json_get_string(body, "name", req_name, sizeof(req_name));
        }

        if(!req_name[0] || strchr(req_name, '/') || strchr(req_name, '\\') || strcmp(req_name, ".") == 0 || strcmp(req_name, "..") == 0) {
            send_json_error(fd, 400, "Bad Request", "Invalid folder name");
            close(fd);
            return NULL;
        }

        char rel_full[1024];
        if(req_dir[0]) snprintf(rel_full, sizeof(rel_full), "%s/%s", req_dir, req_name);
        else snprintf(rel_full, sizeof(rel_full), "%s", req_name);

        char abs_path[1024];
        if(!is_safe_sd_path(rel_full, abs_path, sizeof(abs_path))) {
            send_json_error(fd, 403, "Forbidden", "Access outside SD card forbidden");
            close(fd);
            return NULL;
        }

        struct stat st;
        if(stat(abs_path, &st) == 0) {
            send_json_error(fd, 409, "Conflict", "Item with this name already exists");
            close(fd);
            return NULL;
        }

        if(mkdir(abs_path, 0755) == 0) {
            sync();
            const char *ok_body = "{\"ok\":true}\n";
            send_response_headers(fd, 200, "OK", "application/json", strlen(ok_body));
            write(fd, ok_body, strlen(ok_body));
        } else {
            send_json_error(fd, 500, "Internal Server Error", "Could not create directory");
        }
        close(fd);
        return NULL;
    }

    /* Route: GET /api/system/info */
    if(strcasecmp(method, "GET") == 0 && strcmp(path_part, "/api/system/info") == 0) {
        int batt = -1, volt = -1, charging = 0;
        FILE *f = fopen("/sys/class/power_supply/cw221X-bat/capacity", "r");
        if(f) { if(fscanf(f, "%d", &batt) != 1) batt = -1; fclose(f); }

        f = fopen("/sys/class/power_supply/cw221X-bat/voltage_now", "r");
        if(f) { if(fscanf(f, "%d", &volt) != 1) volt = -1; fclose(f); }

        f = fopen("/sys/class/power_supply/cw221X-bat/status", "r");
        if(f) {
            char st[32] = "";
            if(fgets(st, sizeof(st), f) && strncasecmp(st, "Charging", 8) == 0) charging = 1;
            fclose(f);
        }

        struct statvfs sv;
        unsigned long long sd_free = 0, sd_total = 0;
        if(statvfs(SD_ROOT, &sv) == 0) {
            sd_free = (unsigned long long)sv.f_bavail * sv.f_frsize;
            sd_total = (unsigned long long)sv.f_blocks * sv.f_frsize;
        }

        char sys_json[512];
        snprintf(sys_json, sizeof(sys_json),
            "{\"battery\":%d,\"voltage_now\":%d,\"charging\":%s,\"sd_free\":%llu,\"sd_total\":%llu}\n",
            batt, volt, charging ? "true" : "false", sd_free, sd_total);
        send_response_headers(fd, 200, "OK", "application/json", strlen(sys_json));
        write(fd, sys_json, strlen(sys_json));
        close(fd);
        return NULL;
    }

    /* Route: POST /api/library/rescan */
    if(strcasecmp(method, "POST") == 0 && strcmp(path_part, "/api/library/rescan") == 0) {
        /* Send 0622000C0001 to mq_player to initiate catalog scan */
        send_player_cmd("0622000C0001");
        const char *resp = "{\"ok\":true}\n";
        send_response_headers(fd, 200, "OK", "application/json", strlen(resp));
        write(fd, resp, strlen(resp));
        close(fd);
        return NULL;
    }

    /* Route: GET /api/settings */
    if(strcasecmp(method, "GET") == 0 && strcmp(path_part, "/api/settings") == 0) {
        char pin[64] = "", wm_str[32] = "0", sleep_str[32] = "0";
        get_config_value("web_pw", pin, sizeof(pin), "1234");
        get_config_value("work_mode", wm_str, sizeof(wm_str), "0");
        get_config_value("sleep_idx", sleep_str, sizeof(sleep_str), "0");

        int brightness = 15;
        FILE *bf = fopen("/sys/class/backlight/backlight/brightness", "r");
        if(bf) { if(fscanf(bf, "%d", &brightness) != 1) brightness = 15; fclose(bf); }

        char s_json[512];
        snprintf(s_json, sizeof(s_json),
            "{\"ok\":true,\"has_pin\":%s,\"pin\":\"%s\",\"play_mode\":%d,\"sleep_timer\":%d,\"brightness\":%d,\"max_brightness\":40}\n",
            pin[0] ? "true" : "false", pin, atoi(wm_str), atoi(sleep_str), brightness);
        send_response_headers(fd, 200, "OK", "application/json", strlen(s_json));
        write(fd, s_json, strlen(s_json));
        close(fd);
        return NULL;
    }

    /* Route: POST /api/settings */
    if(strcasecmp(method, "POST") == 0 && strcmp(path_part, "/api/settings") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if(body) body += 4;
        if(!body) body = "";

        char new_pin[64] = "";
        json_get_string(body, "pin", new_pin, sizeof(new_pin));
        if(!new_pin[0]) json_get_string(body, "password", new_pin, sizeof(new_pin));
        if(new_pin[0]) {
            update_config_value("web_pw", new_pin);
            pthread_mutex_lock(&g_auth_mu);
            strncpy(g_password, new_pin, sizeof(g_password) - 1);
            g_password[sizeof(g_password) - 1] = 0;
            pthread_mutex_unlock(&g_auth_mu);
        }

        char *br_p = strstr(body, "\"brightness\":");
        if(br_p) {
            int br = atoi(br_p + 13);
            if(br >= 0 && br <= 40) {
                char br_s[16]; snprintf(br_s, sizeof(br_s), "%d", br);
                update_config_value("brightness", br_s);
                FILE *bf = fopen("/sys/class/backlight/backlight/brightness", "w");
                if(bf) { fprintf(bf, "%d\n", br); fclose(bf); }
            }
        }

        char *pm_p = strstr(body, "\"play_mode\":");
        if(pm_p) {
            int pm = atoi(pm_p + 12);
            if(pm >= 0 && pm <= 4) {
                char pm_s[16]; snprintf(pm_s, sizeof(pm_s), "%d", pm);
                update_config_value("work_mode", pm_s);
                g_cached_work_mode = pm;
                g_last_conf_check = time(NULL);
                char m[32]; snprintf(m, sizeof(m), "0102000C%04X", pm & 0xFFFF);
                send_player_cmd(m);
            }
        }

        char *st_p = strstr(body, "\"sleep_timer\":");
        if(st_p) {
            int st_val = atoi(st_p + 14);
            if(st_val >= 0 && st_val <= 6) {
                char st_s[16]; snprintf(st_s, sizeof(st_s), "%d", st_val);
                update_config_value("sleep_idx", st_s);
            }
        }

        char *act = strstr(body, "\"action\":");
        if(act) {
            if(strstr(act, "\"restart_ui\"")) {
                system("killall -9 mq_ui &");
            } else if(strstr(act, "\"reboot\"")) {
                system("/sbin/reboot &");
            }
        }

        const char *resp = "{\"ok\":true}\n";
        send_response_headers(fd, 200, "OK", "application/json", strlen(resp));
        write(fd, resp, strlen(resp));
        close(fd);
        return NULL;
    }

    /* Route: GET /api/player/art */
    if(strcasecmp(method, "GET") == 0 && strcmp(path_part, "/api/player/art") == 0) {
        const char *art_path = "/usr/data/fiio/cover.png";
        const char *mime = "image/png";
        int afd = open(art_path, O_RDONLY);
        if(afd < 0) {
            art_path = "/usr/data/fiio/cover.jpg";
            mime = "image/jpeg";
            afd = open(art_path, O_RDONLY);
        }
        if(afd >= 0) {
            struct stat st;
            fstat(afd, &st);
            send_static_response_headers(fd, 200, "OK", mime, st.st_size, "public, max-age=60");
            char buf[BUFFER_SIZE];
            ssize_t bytes;
            while((bytes = read(afd, buf, sizeof(buf))) > 0) {
                if(write(fd, buf, bytes) <= 0) break;
            }
            close(afd);
        } else {
            /* 1x1 transparent SVG fallback */
            const char *svg = "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>";
            send_static_response_headers(fd, 200, "OK", "image/svg+xml", strlen(svg), "public, max-age=60");
            write(fd, svg, strlen(svg));
        }
        close(fd);
        return NULL;
    }

    /* Static Asset Serving (index.html, style.css, app.js) */
    if(strcmp(path_part, "/") == 0) {
        strcpy(path_part, "/index.html");
    }

    char file_target[512];
    snprintf(file_target, sizeof(file_target), "%s%s", g_web_dir, path_part);

    int sfd = open(file_target, O_RDONLY);
    if(sfd < 0 && strcmp(g_web_dir, DEFAULT_WEB_DIR) == 0) {
        /* Try fallback directory ./web */
        snprintf(file_target, sizeof(file_target), "%s%s", FALLBACK_WEB_DIR, path_part);
        sfd = open(file_target, O_RDONLY);
    }

    if(sfd >= 0) {
        struct stat st;
        fstat(sfd, &st);
        const char *mime = get_mime_type(file_target);
        const char *cc = "no-cache";
        if(strstr(file_target, ".css") || strstr(file_target, ".js") || strstr(file_target, ".svg") || strstr(file_target, ".png")) {
            cc = "public, max-age=86400";
        }
        send_static_response_headers(fd, 200, "OK", mime, st.st_size, cc);
        char buf[BUFFER_SIZE];
        ssize_t bytes;
        while((bytes = read(sfd, buf, sizeof(buf))) > 0) {
            if(write(fd, buf, bytes) <= 0) break;
        }
        close(sfd);
    } else {
        send_json_error(fd, 404, "Not Found", "Resource not found");
    }

    close(fd);
    return NULL;
}

static void cleanup_pid_files(void) {
    unlink("/tmp/diskos-webd.pid");
    unlink("/tmp/diskos_ws_active");
}

static void sig_handler(int sig) {
    (void)sig;
    cleanup_pid_files();
    _exit(0);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    atexit(cleanup_pid_files);
    srand((unsigned int)time(NULL));

    FILE *pf = fopen("/tmp/diskos-webd.pid", "w");
    if(pf) {
        fprintf(pf, "%d\n", getpid());
        fclose(pf);
    }

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if(strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            strncpy(g_web_dir, argv[++i], sizeof(g_web_dir) - 1);
        } else if(strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            strncpy(g_conf_path, argv[++i], sizeof(g_conf_path) - 1);
        } else if(strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            strncpy(g_password, argv[++i], sizeof(g_password) - 1);
        }
    }

    load_config_password();
    printf("diskOS Web Server starting on port %d (web dir: %s)\n", g_port, g_web_dir);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_port);

    if(bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if(listen(server_fd, 16) != 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 262144); /* 256KB stack per client */

    while(1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if(client_fd < 0) {
            if(errno == EINTR) continue;
            perror("accept");
            break;
        }

        pthread_t th;
        if(pthread_create(&th, &attr, handle_client, (void *)(intptr_t)client_fd) != 0) {
            close(client_fd);
        }
    }

    pthread_attr_destroy(&attr);
    close(server_fd);
    return 0;
}

