/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_REMOTE_BROWSE_H
#define DISKOS_REMOTE_BROWSE_H
#include "remote_discovery.h"
#include "cast_discovery.h"
int remote_browse_run(cast_disc_job *, remote_disc *, unsigned ms);
#endif
