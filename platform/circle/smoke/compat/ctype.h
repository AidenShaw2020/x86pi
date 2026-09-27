// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/* ini.c only needs the ASCII whitespace subset. Keep this header deliberately
 * narrow so the smoke target does not pull a host ctype implementation. */
static inline int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}
