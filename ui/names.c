/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "names.h"
#include <string.h>

names_result_t names_check(names_field_t field, const char *text, const char *stored){
    if(text && stored && !strcmp(text, stored)) return NAMES_OK;
    if(!text || !text[0]) return NAMES_EMPTY;
    size_t n = strlen(text);
    if(n > NAMES_MAX_BYTES) return NAMES_TOO_LONG;
    unsigned words = 0;
    for(size_t i = 0; i < n; i++){
        unsigned char c = (unsigned char)text[i];
        if(c >= 128) return NAMES_ASCII;
        if(c < 32 || c == 127) return NAMES_CONTROL;
        if(c != ' ' && (i == 0 || text[i-1] == ' ')) words++;
    }
    if(!words) return NAMES_EMPTY;
    if(field == NAMES_NETWORK){
        /* One ASCII DNS label; letter case is kept as typed, with a smaller product limit of 32 bytes.
         * Stock defaults pass only as unchanged stored values, never overrides. */
        for(size_t i = 0; i < n; i++){
            char c = text[i];
            if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-'))
                return NAMES_HOST_CHARS;
        }
        if(text[0] == '-' || text[n-1] == '-') return NAMES_HOST_EDGE;
    } else if(words > 9) return NAMES_WORDS;
    return NAMES_OK;
}

names_result_t names_bt_normalize(const char *text, char out[NAMES_MAX_BYTES + 1]){
    names_result_t r = names_check(NAMES_BT, text, NULL);
    if(r != NAMES_OK) return r;
    size_t n = 0;
    int space = 0;
    for(; *text; text++){
        if(*text == ' '){ space = n != 0; continue; }
        if(space) out[n++] = ' ';
        out[n++] = *text;
        space = 0;
    }
    out[n] = 0;
    return NAMES_OK;
}

const char *names_reason(names_result_t result){
    switch(result){
    case NAMES_OK: return NULL;
    case NAMES_EMPTY: return "Enter a name";
    case NAMES_TOO_LONG: return "Use 1 to 32 bytes";
    case NAMES_ASCII: return "Use ASCII characters";
    case NAMES_CONTROL: return "Remove control characters";
    case NAMES_HOST_CHARS: return "Use letters, digits or -";
    case NAMES_HOST_EDGE: return "Start/end: a letter or digit";
    case NAMES_WORDS: return "Use at most 9 words";
    }
    return "Invalid name";
}

/* Bounded D-Bus Alias writes and readback live in btalias.c. */

int names_saved_bt_valid(const char *s){
    size_t n = s ? strlen(s) : 0;
    if(!n || n > NAMES_STORED_BYTES) return 0;
    /* Strict UTF-8, including overlong, surrogate and out-of-range rejection. */
    for(size_t i = 0; i < n;){
        unsigned c = (unsigned char)s[i++], value, count, min;
        if(c < 128){ if(c < 32 || c == 127) return 0; continue; }
        if(c >= 0xc2 && c <= 0xdf){ value=c&31; count=1; min=0x80; }
        else if(c >= 0xe0 && c <= 0xef){ value=c&15; count=2; min=0x800; }
        else if(c >= 0xf0 && c <= 0xf4){ value=c&7; count=3; min=0x10000; }
        else return 0;
        if(i+count > n) return 0;
        while(count--){ c=(unsigned char)s[i++]; if((c&0xc0)!=0x80) return 0; value=(value<<6)|(c&63); }
        if(value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}
