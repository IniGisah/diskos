/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "receiver.h"
#include "fwcaps.h"
#include <stddef.h>

static receiver_snapshot g_receiver = {
    RECEIVER_UNCERTAIN, RECEIVER_SOURCE_UNKNOWN, RECEIVER_SOURCE_UNKNOWN, 0, 0, 0
};

static uint64_t g_arm_id, g_obs_id;
static int g_observe = 1;

int receiver_available(void){ return fw_airplay_receiver(); }
const char *receiver_entry_blocked_reason(void){ return NULL; }

void receiver_uncertain(void){
    ipc_receiver_end();
    ipc_source_cancel_arm(g_arm_id); g_arm_id=0;
    ipc_source_cancel_arm(g_obs_id); g_obs_id=0;
    g_receiver.phase=RECEIVER_UNCERTAIN;
    g_receiver.source=g_receiver.target=RECEIVER_SOURCE_UNKNOWN;
}

void receiver_get_snapshot(receiver_snapshot *out){
    ipc_source_evidence e;
    ipc_source_snapshot(&e);
    if(!receiver_available() || !e.valid || e.generation!=g_receiver.generation){
        receiver_uncertain();
        g_receiver.generation=e.generation;
        g_receiver.source_sequence=e.sequence;
    } else {
        if(g_receiver.phase==RECEIVER_PENDING_ARM && e.arm_id==g_arm_id){
            g_receiver.source_sequence=e.arm_sequence;
            g_receiver.phase=RECEIVER_PENDING;
        }
        if(g_receiver.phase==RECEIVER_SELECTED &&
           e.mode_change_sequence!=g_receiver.mode_change_sequence) receiver_uncertain();
        if(g_receiver.phase==RECEIVER_UNCERTAIN && g_obs_id && e.adopt_id==g_obs_id && e.mode==10){
            /* Observation adopted an already selected AirPlay source from fresh evidence (ipc.c). */
            g_receiver.source=g_receiver.target=RECEIVER_SOURCE_AIRPLAY;
            g_receiver.phase=RECEIVER_SELECTED;
            g_receiver.mode_change_sequence=e.mode_change_sequence;
            g_receiver.source_sequence=e.sequence;
        }
        if(g_receiver.phase!=RECEIVER_PENDING_ARM && e.sequence>g_receiver.source_sequence){
            /* V2.40/V2.57 external wire modes: 8 = Local, 0A = AirPlay. A2 work_mode
             * strings and the player's internal enum 6 are not source evidence. */
            receiver_source source=e.mode==8 ? RECEIVER_SOURCE_LOCAL :
                e.mode==10 ? RECEIVER_SOURCE_AIRPLAY : RECEIVER_SOURCE_UNKNOWN;
            if(g_receiver.phase==RECEIVER_PENDING && source==g_receiver.target){
                g_receiver.source=source;
                g_receiver.phase=RECEIVER_SELECTED;
                g_receiver.mode_change_sequence=e.mode_change_sequence;
            } else if(g_receiver.phase==RECEIVER_SELECTED && source!=g_receiver.source){
                receiver_uncertain();
            }
            g_receiver.source_sequence=e.sequence;
        }
    }
    /* No transition armed and nothing selected: watch for an already selected source (UI restart, player replacement). */
    if(g_observe && g_receiver.phase==RECEIVER_UNCERTAIN && !g_arm_id && !g_obs_id && receiver_available() && e.valid)
        g_obs_id=ipc_observe_arm(e.generation);
    *out=g_receiver;
}
/* The caller owns the receiver (a transition or a failed one): adoption is off and a pending observation is dropped.
 * A selected, adopted session keeps its boundary: it is the ownership proof. */
void receiver_observe(int on){
    g_observe=on;
    if(!on && g_obs_id && g_receiver.phase==RECEIVER_UNCERTAIN){ ipc_source_cancel_arm(g_obs_id); g_obs_id=0; }
}

int receiver_expect(receiver_source target){
    receiver_snapshot st;
    unsigned generation;
    receiver_get_snapshot(&st);
    if(!receiver_available() || (st.phase==RECEIVER_PENDING_ARM || st.phase==RECEIVER_PENDING) ||
       (target!=RECEIVER_SOURCE_LOCAL && target!=RECEIVER_SOURCE_AIRPLAY) ||
       !ipc_confirmed_snapshot(&generation)) return 0;
    g_arm_id=ipc_source_arm(generation);
    if(!g_arm_id) return 0;
    g_obs_id=0;   /* the transition arm replaced the observation boundary */
    if(target==RECEIVER_SOURCE_AIRPLAY) ipc_receiver_begin(generation,g_arm_id);
    else ipc_receiver_return(generation,g_arm_id);
    g_receiver.phase=RECEIVER_PENDING_ARM;
    g_receiver.source=RECEIVER_SOURCE_UNKNOWN; g_receiver.target=target;
    g_receiver.generation=generation;
    return 1;
}

void receiver_get_metadata(ipc_receiver_meta *out){
    receiver_snapshot state;
    receiver_get_snapshot(&state);
    ipc_receiver_metadata(out);
}
int receiver_art_equal(const receiver_art_key *a, const receiver_art_key *b){
    return a->generation==b->generation && a->session==b->session &&
        a->item==b->item && a->revision==b->revision && a->present==b->present;
}
int receiver_art_current(const receiver_art_key *key){
    receiver_art_key current; receiver_art_get(&current);
    return key->present && receiver_art_equal(key,&current);
}
/* Slice 5: read-only receiving UI. */
