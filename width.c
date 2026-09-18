/* Display width of UTF-8 text on a terminal.
 *
 * Widths are per grapheme cluster rather than per codepoint: a cluster takes the width of its base
 * character, which the emoji rules can override.
 * That is where terminals converged, and it answers two questions with one rule, since the same
 * boundaries decide where a line may break.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "width.h"

#include <stdio.h>
#include <string.h>

#include "unicode_tables.h"

static int ambiguous_wide = 0;

void wu_set_ambiguous_wide(int wide) { ambiguous_wide = wide ? 1 : 0; }

/* The property word of a codepoint; zero for everything the tables leave out. */
static unsigned props_of(unsigned cp) {
  size_t lo = 0, hi = sizeof unicode_ranges / sizeof unicode_ranges[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const struct urange *r = &unicode_ranges[mid];
    if (cp < r->lo) {
      hi = mid;
    } else if (cp > r->lo + r->extra) {
      lo = mid + 1;
    } else {
      return r->props;
    }
  }
  return 0;
}

size_t wu_decode(const char *s, size_t n, size_t i, unsigned *cp) {
  unsigned char c = (unsigned char)s[i];
  size_t need;
  unsigned value;
  if (c < 0x80) {
    *cp = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    need = 2;
    value = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    need = 3;
    value = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    need = 4;
    value = c & 0x07;
  } else {
    *cp = c;
    return 1;
  }
  if (i + need > n) {
    *cp = c;
    return 1;
  }
  for (size_t k = 1; k < need; k++) {
    unsigned char cont = (unsigned char)s[i + k];
    if ((cont & 0xC0) != 0x80) {
      *cp = c;
      return 1;
    }
    value = (value << 6) | (cont & 0x3F);
  }
  *cp = value;
  return need;
}

/* ---------- UAX #29 extended grapheme clusters ---------- */

/* The state a boundary decision needs beyond the two adjacent codepoints. */
typedef struct {
  int regional_indicators; // GB12/GB13: a pair joins, a third starts over
  int emoji_zwj;           // GB11: an Extended_Pictographic seen through Extend* ZWJ
  int linker_run;          // GB9.3: a ConjunctLinker, seen through any ConjunctExtenders after it
} ClusterState;

static int is_control(unsigned gcb) {
  return gcb == GCB_CONTROL || gcb == GCB_CR || gcb == GCB_LF;
}

/* Whether a break falls between `before` and `after`. */
static int breaks_between(unsigned pw, unsigned nw, ClusterState *st) {
  unsigned pg = UPROP_GCB(pw), ng = UPROP_GCB(nw);

  if (pg == GCB_CR && ng == GCB_LF) return 0;                              // GB3
  if (is_control(pg) || is_control(ng)) return 1;                          // GB4, GB5
  // GB6, GB7, GB8: a Hangul syllable holds together.
  if (pg == GCB_L && (ng == GCB_L || ng == GCB_V || ng == GCB_LV || ng == GCB_LVT)) return 0;
  if ((pg == GCB_LV || pg == GCB_V) && (ng == GCB_V || ng == GCB_T)) return 0;
  if ((pg == GCB_LVT || pg == GCB_T) && ng == GCB_T) return 0;
  if (ng == GCB_EXTEND || ng == GCB_ZWJ) return 0;                        // GB9
  if (ng == GCB_SPACINGMARK) return 0;                                    // GB9a
  if (pg == GCB_PREPEND) return 0;                                        // GB9b
  if (st->linker_run && UPROP_INCB(nw) == INCB_CONSONANT) return 0;      // GB9.3
  if (st->emoji_zwj && pg == GCB_ZWJ && UPROP_EXTPICT(nw)) return 0;      // GB11
  // GB12, GB13: regional indicators pair up, so only an odd one joins the next.
  if (pg == GCB_REGIONAL_INDICATOR && ng == GCB_REGIONAL_INDICATOR &&
      st->regional_indicators % 2 == 1)
    return 0;
  return 1;                                                               // GB999
}

/* Fold one codepoint into the state that later boundary decisions read. */
static void advance_state(unsigned w, ClusterState *st) {
  unsigned gcb = UPROP_GCB(w), incb = UPROP_INCB(w);

  st->regional_indicators = gcb == GCB_REGIONAL_INDICATOR ? st->regional_indicators + 1 : 0;

  if (UPROP_EXTPICT(w)) st->emoji_zwj = 1;
  else if (gcb != GCB_EXTEND && gcb != GCB_ZWJ) st->emoji_zwj = 0;

  /* A linker opens the run and conjunct extenders keep it open; anything else closes it. */
  if (incb == INCB_LINKER) st->linker_run = 1;
  else if (!(st->linker_run && incb == INCB_EXTEND)) st->linker_run = 0;
}

size_t wu_cluster_end(const char *s, size_t n, size_t i) {
  if (i >= n) return i;
  ClusterState st = {0, 0, 0};
  unsigned cp;
  size_t len = wu_decode(s, n, i, &cp);
  unsigned pw = props_of(cp);
  advance_state(pw, &st);
  size_t pos = i + len;
  while (pos < n) {
    unsigned next;
    size_t next_len = wu_decode(s, n, pos, &next);
    unsigned nw = props_of(next);
    /* GB9.3 reads a linker run, which is state rather than the single previous codepoint. */
    if (breaks_between(pw, nw, &st)) break;
    advance_state(nw, &st);
    pw = nw;
    pos += next_len;
  }
  return pos;
}

int wu_cluster_width(const char *s, size_t n, size_t i, size_t end) {
  if (i >= end) return 0;
  unsigned base = 0;
  int have_base = 0, vs16 = 0, vs15 = 0, regional = 0, emoji = 0;
  for (size_t pos = i; pos < end;) {
    unsigned cp;
    pos += wu_decode(s, n, pos, &cp);
    unsigned w = props_of(cp);
    if (cp == 0xFE0F) vs16 = 1;
    if (cp == 0xFE0E) vs15 = 1;
    if (UPROP_GCB(w) == GCB_REGIONAL_INDICATOR) regional++;
    if (UPROP_EXTPICT(w) && UPROP_EMOJIPRES(w)) emoji = 1;
    /* The base is the first codepoint that is not a prefix of one. */
    if (!have_base && UPROP_GCB(w) != GCB_PREPEND) {
      base = w;
      have_base = 1;
    }
  }
  if (vs16) return 2;                       // an emoji presentation selector forces two columns
  if (regional >= 2) return 2;              // a flag is one double-width glyph
  if (emoji && !vs15) return 2;
  switch (UPROP_WIDTH(base)) {
    case UWIDTH_WIDE: return 2;
    case UWIDTH_AMBIGUOUS: return ambiguous_wide ? 2 : 1;
    case UWIDTH_ZERO: return 0;
    default: break;
  }
  return is_control(UPROP_GCB(base)) ? 0 : 1;
}

/* ---------- ANSI escape sequences ---------- */

size_t wu_escape_len(const char *s, size_t n, size_t i) {
  if (i >= n || (unsigned char)s[i] != 0x1B) return 0;
  if (i + 1 >= n) return 1;
  unsigned char kind = (unsigned char)s[i + 1];
  size_t pos = i + 2;
  if (kind == '[') {
    /* CSI: parameter and intermediate bytes, then one final byte. */
    while (pos < n && (unsigned char)s[pos] >= 0x20 && (unsigned char)s[pos] <= 0x3F) pos++;
    while (pos < n && (unsigned char)s[pos] >= 0x20 && (unsigned char)s[pos] <= 0x2F) pos++;
    if (pos < n) pos++;
    return pos - i;
  }
  if (kind == ']') {
    /* OSC: runs to BEL or to the two-byte string terminator. */
    while (pos < n) {
      if ((unsigned char)s[pos] == 0x07) return pos + 1 - i;
      if ((unsigned char)s[pos] == 0x1B && pos + 1 < n && s[pos + 1] == '\\') return pos + 2 - i;
      pos++;
    }
    return n - i;
  }
  return 2; // a two-byte escape
}

size_t wu_display_width(const char *s, size_t n) {
  size_t width = 0, pos = 0;
  while (pos < n) {
    size_t esc = wu_escape_len(s, n, pos);
    if (esc) {
      pos += esc;
      continue;
    }
    size_t end = wu_cluster_end(s, n, pos);
    width += (size_t)wu_cluster_width(s, n, pos, end);
    pos = end;
  }
  return width;
}

/* ---------- select graphic rendition ---------- */

/* The attributes worth carrying across a break; the numbers are their SGR codes. */
static const struct {
  unsigned bit;
  int set;
  int clear;
} attr_codes[] = {
  {1u << 0, 1, 22}, {1u << 1, 2, 22}, {1u << 2, 3, 23},
  {1u << 3, 4, 24}, {1u << 4, 7, 27}, {1u << 5, 9, 29},
};

void wu_sgr_clear(WuSgr *st) {
  st->fg[0] = '\0';
  st->bg[0] = '\0';
  st->attrs = 0;
}

int wu_sgr_active(const WuSgr *st) {
  return st->fg[0] != '\0' || st->bg[0] != '\0' || st->attrs != 0;
}

/* Copy one parameter run (which may carry the 5;N or 2;R;G;B forms) into a colour slot. */
static void store_color(char *slot, size_t cap, const char *params, size_t len) {
  if (len >= cap) len = cap - 1;
  memcpy(slot, params, len);
  slot[len] = '\0';
}

/* Apply one CSI ... m sequence, whose parameter bytes are [p, p + len). */
static void apply_sgr(WuSgr *st, const char *p, size_t len) {
  size_t i = 0;
  if (len == 0) {
    wu_sgr_clear(st);
    return;
  }
  while (i < len) {
    size_t start = i;
    long code = 0;
    int digits = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
      code = code * 10 + (p[i] - '0');
      digits++;
      i++;
    }
    if (!digits) code = 0;
    /* The 256-colour and truecolour forms swallow the parameters that follow them. */
    if ((code == 38 || code == 48) && i < len && p[i] == ';') {
      size_t run = i + 1;
      long selector = 0;
      while (run < len && p[run] >= '0' && p[run] <= '9')
        selector = selector * 10 + (p[run++] - '0');
      int extra = selector == 5 ? 1 : selector == 2 ? 3 : 0;
      for (int k = 0; k < extra && run < len && p[run] == ';'; k++) {
        run++;
        while (run < len && p[run] >= '0' && p[run] <= '9') run++;
      }
      store_color(code == 38 ? st->fg : st->bg, sizeof st->fg, p + start, run - start);
      i = run;
    } else if (code == 0) {
      wu_sgr_clear(st);
    } else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
      store_color(st->fg, sizeof st->fg, p + start, i - start);
    } else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107)) {
      store_color(st->bg, sizeof st->bg, p + start, i - start);
    } else if (code == 39) {
      st->fg[0] = '\0';
    } else if (code == 49) {
      st->bg[0] = '\0';
    } else {
      for (size_t k = 0; k < sizeof attr_codes / sizeof attr_codes[0]; k++) {
        if (code == attr_codes[k].set) st->attrs |= attr_codes[k].bit;
        if (code == attr_codes[k].clear) st->attrs &= ~attr_codes[k].bit;
      }
    }
    if (i < len && p[i] == ';') i++;
    else if (i < len && p[i] != ';') i++; // skip a byte no parameter form claims
  }
}

void wu_sgr_scan(WuSgr *st, const char *s, size_t n) {
  size_t pos = 0;
  while (pos < n) {
    size_t esc = wu_escape_len(s, n, pos);
    if (!esc) {
      pos++;
      continue;
    }
    /* Only CSI ... m carries rendition; every other sequence passes through untouched. */
    if (esc >= 3 && s[pos + 1] == '[' && s[pos + esc - 1] == 'm') {
      apply_sgr(st, s + pos + 2, esc - 3);
    }
    pos += esc;
  }
}

void wu_sgr_render(const WuSgr *st, char *out, size_t cap) {
  out[0] = '\0';
  if (!wu_sgr_active(st)) return;
  char params[128];
  size_t len = 0;
  for (size_t k = 0; k < sizeof attr_codes / sizeof attr_codes[0]; k++) {
    if (!(st->attrs & attr_codes[k].bit)) continue;
    len += (size_t)snprintf(params + len, sizeof params - len, "%s%d",
                            len ? ";" : "", attr_codes[k].set);
  }
  if (st->fg[0])
    len += (size_t)snprintf(params + len, sizeof params - len, "%s%s", len ? ";" : "", st->fg);
  if (st->bg[0])
    len += (size_t)snprintf(params + len, sizeof params - len, "%s%s", len ? ";" : "", st->bg);
  snprintf(out, cap, "\033[%sm", params);
}
