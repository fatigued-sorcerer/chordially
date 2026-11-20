// SPDX-License-Identifier: GPL-3.0-only
#ifndef CM_PHRASES_H
#define CM_PHRASES_H
int ph_nstyles(void);                 /* including 0 = OFF */
const char *ph_style(int s);
const char *ph_var_name(int s, int v);
const char *ph_text(int s, int v);    /* NULL for OFF */
#endif
