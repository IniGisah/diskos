/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_REMOTE_BACKEND_H
#define DISKOS_REMOTE_BACKEND_H
/* Output identity, shared with remote workers without importing local IPC state.
 * Existing values are stable; the transport owner publishes one effective source. */
typedef enum { PB_SRC_LOCAL = 0, PB_SRC_CAST, PB_SRC_AIRPLAY, PB_SRC_COUNT } pb_source;
#endif
