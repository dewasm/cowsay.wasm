/* Display width of UTF-8 text on a terminal.
 * Grapheme clusters, East Asian width, and the SGR escapes that survive a line break.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef COWSAY_WIDTH_H
#define COWSAY_WIDTH_H

#include <stddef.h>

/* Whether East Asian Ambiguous characters take two columns; one by default, per Unicode. */
void wu_set_ambiguous_wide(int wide);

/* Decode the UTF-8 sequence at `i`, returning its byte length and writing the codepoint.
 * An invalid lead byte decodes as itself over one byte, so arbitrary input still moves on. */
size_t wu_decode(const char *s, size_t n, size_t i, unsigned *cp);

/* The end of the extended grapheme cluster that starts at `i` (UAX #29). */
size_t wu_cluster_end(const char *s, size_t n, size_t i);

/* The column count of the cluster spanning [i, end). */
int wu_cluster_width(const char *s, size_t n, size_t i, size_t end);

/* The length of the ANSI escape sequence at `i`, or zero when none starts there. */
size_t wu_escape_len(const char *s, size_t n, size_t i);

/* The columns `s` occupies: clusters counted by width, escape sequences counted as nothing. */
size_t wu_display_width(const char *s, size_t n);

/* The select-graphic-rendition state a line ends in, so the next line can open with it. */
typedef struct {
  char fg[32];
  char bg[32];
  unsigned attrs;
} WuSgr;

void wu_sgr_clear(WuSgr *st);

/* Apply every SGR sequence in `s` to the state. */
void wu_sgr_scan(WuSgr *st, const char *s, size_t n);

/* Whether the state is anything other than the terminal's default. */
int wu_sgr_active(const WuSgr *st);

/* Write the escape sequence that reinstates the state, NUL-terminated;
 * empty when the state is the default. */
void wu_sgr_render(const WuSgr *st, char *out, size_t cap);

#endif
