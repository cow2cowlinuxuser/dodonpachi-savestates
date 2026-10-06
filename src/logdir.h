#ifndef D3D9_SW_LOGDIR_H
#define D3D9_SW_LOGDIR_H

#include <stdio.h>
#include <windows.h>

/* Every log of one launch goes in one folder of its own,
 * <game folder>\logs\<yyyy-mm-dd>_<hh-mm-ss>_<exe>_<pid>, named from the
 * process's start time so every writer in the process agrees on it. A session
 * is sent by copying its folder, and the next launch cannot overwrite it. */
const char *swlog_dir(void);
/* <session folder>\<name>, creating any subfolder in name. */
int swlog_path(const char *name, char *out, unsigned cap);
/* Opened in the session folder; a file that is still empty gets the header. */
FILE *swlog_fopen(const char *name, const char *mode);
HANDLE swlog_create(const char *name, DWORD creation);
/* The header text: what launch, build and machine wrote this log. */
int swlog_header(char *out, int cap, const char *name);

#endif
