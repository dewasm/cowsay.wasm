/*
 * cowsay.wasm: a C reimplementation of cowsay 3.03 (c) 1999-2000 Tony Monroe, for wasm32-wasip1.
 *
 * The output specification and the deliberate fixes are documented in test/README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cows_embedded.h"

#define VERSION "3.03"

/* ---------- growable byte buffer and string list ---------- */

typedef struct {
  char *p;
  size_t len, cap;
} Buf;

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) {
    fputs("cowsay: out of memory\n", stderr);
    exit(1);
  }
  return q;
}

static void buf_append(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->p = xrealloc(b->p, b->cap);
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}

static void buf_push(Buf *b, char c) { buf_append(b, &c, 1); }

static void buf_repeat(Buf *b, char c, size_t n) {
  for (size_t i = 0; i < n; i++) buf_push(b, c);
}

static char *xstrndup(const char *s, size_t n) {
  char *p = xrealloc(NULL, n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

typedef struct {
  char **v;
  size_t n, cap;
} List;

static void list_push(List *l, char *s) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->v = xrealloc(l->v, l->cap * sizeof(char *));
  }
  l->v[l->n++] = s;
}

/* ---------- UTF-8 units ----------
 *
 * A unit is one codepoint.
 * A byte that starts no valid sequence (stray continuation, truncated) is one unit by itself.
 * Overlong encodings pass: the goal is never to split a sequence, not to validate it.
 */

static size_t u8_len_at(const char *s, size_t n, size_t i) {
  unsigned char c = (unsigned char)s[i];
  size_t need;
  if (c < 0x80) need = 1;
  else if ((c & 0xE0) == 0xC0) need = 2;
  else if ((c & 0xF0) == 0xE0) need = 3;
  else if ((c & 0xF8) == 0xF0) need = 4;
  else return 1;
  if (i + need > n) return 1;
  for (size_t k = 1; k < need; k++) {
    if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 1;
  }
  return need;
}

static size_t u8_count(const char *s, size_t n) {
  size_t i = 0, units = 0;
  while (i < n) {
    i += u8_len_at(s, n, i);
    units++;
  }
  return units;
}

/* First `units` codepoints of s, as a fresh string: Perl substr($s, 0, 2) lifted to codepoints. */
static char *u8_prefix(const char *s, size_t units) {
  size_t n = strlen(s), i = 0;
  while (units > 0 && i < n) {
    i += u8_len_at(s, n, i);
    units--;
  }
  return xstrndup(s, i);
}

/* ---------- Text::Wrap port ----------
 *
 * A restricted port of Text::Wrap 2024.001 wrap() and fill().
 * The settings are fixed: ip = xp = "", $break = whitespace, $huge = 'wrap', $separator = "\n".
 * cowsay never feeds it a tab: fill collapses them, and the -l listing contains none.
 * The original's expand/unexpand round trip is therefore a no-op, and is omitted.
 * Units are codepoints where the byte-mode original counts bytes.
 */

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static char *wrap_text(const char *t, size_t n, long *columns) {
  // Deliberate fix: below two columns the reference feeds Text::Wrap a negative regex quantifier;
  // it then returns its argument count, so the message becomes "3".
  // Any smaller width behaves as 2, silently, matching Text::Wrap's own stated minimum.
  if (*columns < 2) *columns = 2;
  long ll = *columns - 1;
  Buf r = {0};
  const char *rem = NULL; // the last consumed break ($remainder)
  int first = 1;
  size_t pos = 0;
  for (;;) {
    // Loop condition /\G(?:$break)*\Z/: stop once only breaks remain.
    size_t p = pos;
    while (p < n && is_space(t[p])) p++;
    if (p == n) break;
    // Greedy match: the longest prefix of at most ll units ending at a break or end of input.
    size_t q = pos;
    long units = 0;
    size_t best = (size_t)-1;
    if (is_space(t[pos])) best = pos; // zero-unit candidate
    while (units < ll && q < n && t[q] != '\n') {
      q += u8_len_at(t, n, q);
      units++;
      if (q == n || is_space(t[q])) best = q;
    }
    if (best != (size_t)-1) {
      if (!first) buf_push(&r, '\n');
      buf_append(&r, t + pos, best - pos);
      if (best < n) {
        rem = t + best; // one break character
        pos = best + 1;
      } else {
        rem = NULL;
        pos = n;
      }
    } else if (ll >= 1 && units == ll) {
      // $huge = 'wrap': cut the overlong word at exactly ll units.
      if (!first) buf_push(&r, '\n');
      buf_append(&r, t + pos, q - pos);
      rem = NULL; // separator; never survives to the end of the loop
      pos = q;
    } else {
      // With ll >= 1 the walk ends at a break candidate or at ll units, so this is unreachable.
      fputs("cowsay: internal wrap error\n", stderr);
      exit(70);
    }
    first = 0;
  }
  // $r .= $remainder: a trailing single break survives, so wrap("a ") is "a ".
  // Breaks skipped by the loop condition are dropped, so wrap(" ") is "".
  if (rem) buf_push(&r, *rem);
  if (!r.p) buf_append(&r, "", 0);
  return r.p;
}

static char *fill_lines(char **lines, size_t nlines, long *columns) {
  // join("\n", @raw)
  Buf t = {0};
  for (size_t i = 0; i < nlines; i++) {
    if (i) buf_push(&t, '\n');
    buf_append(&t, lines[i], strlen(lines[i]));
  }
  // split(/\n\s+/, $t): a newline followed by whitespace separates paragraphs;
  // Perl split keeps a leading empty field and drops trailing empty fields.
  List paras = {0};
  size_t start = 0, i = 0, n = t.len;
  const char *s = t.p ? t.p : "";
  while (i < n) {
    if (s[i] == '\n' && i + 1 < n && is_space(s[i + 1])) {
      list_push(&paras, xstrndup(s + start, i - start));
      i++;
      while (i < n && is_space(s[i])) i++;
      start = i;
    } else {
      i++;
    }
  }
  if (start < n) list_push(&paras, xstrndup(s + start, n - start));
  // (a field that would be empty here is a trailing empty: dropped)

  Buf out = {0};
  for (size_t k = 0; k < paras.n; k++) {
    // $pp =~ s/\s+/ /g
    Buf pp = {0};
    const char *q = paras.v[k];
    for (size_t j = 0; q[j];) {
      if (is_space(q[j])) {
        buf_push(&pp, ' ');
        while (q[j] && is_space(q[j])) j++;
      } else {
        buf_push(&pp, q[j++]);
      }
    }
    char *w = wrap_text(pp.p ? pp.p : "", pp.len, columns);
    if (k) buf_append(&out, "\n\n", 2);
    buf_append(&out, w, strlen(w));
    free(w);
    free(pp.p);
  }
  free(t.p);
  if (!out.p) buf_append(&out, "", 0);
  return out.p;
}

/* Text::Tabs expand() with tabstop 8: pad each tab out to the next multiple of 8.
 * The original tracks only the previous segment's length.
 * That works because the padding realigns every boundary to a tabstop;
 * the segment length modulo 8 then equals the column modulo 8.
 */
static char *expand_line(const char *s) {
  Buf out = {0};
  size_t seg_units = 0;
  for (size_t i = 0; s[i];) {
    if (s[i] == '\t') {
      buf_repeat(&out, ' ', 8 - seg_units % 8);
      seg_units = 0;
      i++;
    } else {
      size_t l = u8_len_at(s, strlen(s), i);
      buf_append(&out, s + i, l);
      seg_units++;
      i += l;
    }
  }
  if (!out.p) buf_append(&out, "", 0);
  return out.p;
}

/* ---------- option parsing: Getopt::Std getopts() port ---------- */

static const char OPTSTRING[] = "bde:f:ghlLnNpstT:wW:y";

typedef struct {
  const char *e, *f, *T, *W;
  int b, d, g, p, s, t, w, y, h, l, n;
} Opts;

/* Consumes options from argv[1..]; returns the index of the first remaining argument.
 * Mirrors Getopt::Std: clustered flags, and a "--" terminator;
 * that includes a terminator produced by rewriting, as in "-b-".
 * An unknown option warns "Unknown option: X" and parsing continues.
 * A missing option argument becomes the empty string, without a message.
 */
static int getopts(int argc, char **argv, Opts *o) {
  int ai = 1;
  const char *cluster = NULL;
  for (;;) {
    if (!cluster) {
      if (ai >= argc || argv[ai][0] != '-' || argv[ai][1] == '\0') break;
      cluster = argv[ai++] + 1;
    }
    if (cluster[0] == '-' && cluster[1] == '\0') break; // "--"
    char c = cluster[0];
    const char *rest = cluster + 1;
    const char *hit = (c != ':') ? strchr(OPTSTRING, c) : NULL;
    if (hit) {
      if (hit[1] == ':') {
        const char *val;
        if (*rest)
          val = rest;
        else if (ai < argc)
          val = argv[ai++];
        else
          val = ""; // ++$errs, value stays undef
        switch (c) {
          case 'e': o->e = val; break;
          case 'f': o->f = val; break;
          case 'T': o->T = val; break;
          case 'W': o->W = val; break;
        }
        cluster = NULL;
      } else {
        switch (c) {
          case 'b': o->b = 1; break;
          case 'd': o->d = 1; break;
          case 'g': o->g = 1; break;
          case 'h': o->h = 1; break;
          case 'l': o->l = 1; break;
          case 'n': o->n = 1; break;
          case 'p': o->p = 1; break;
          case 's': o->s = 1; break;
          case 't': o->t = 1; break;
          case 'w': o->w = 1; break;
          case 'y': o->y = 1; break;
          default: break; // L, N: accepted and ignored, as upstream
        }
        cluster = *rest ? rest : NULL;
      }
    } else {
      // Getopt::Std also handles --help/--version, whose output embeds the host Perl version.
      // They are outside the specification (test/README.md), so they fall through as unknown.
      fprintf(stderr, "Unknown option: %c\n", c);
      cluster = *rest ? rest : NULL;
    }
  }
  return ai;
}

/* ---------- globals mirroring the Perl script ---------- */

static const char *argv0 = "cowsay";
static const char *progname = "cowsay";
static char *eyes;
static char *tongue;
static const char *thoughts;

static void display_usage(void) {
  // Deliberate fix: the reference exits 255 (Perl die), which WASI preview 1 cannot represent.
  // This exits with EX_USAGE, and drops the stray trailing space from its usage text.
  fprintf(stderr,
      "cow{say,think} version " VERSION ", (c) 1999 Tony Monroe\n"
      "Usage: %s [-bdgpstwy] [-h] [-e eyes] [-f cowfile]\n"
      "          [-l] [-n] [-T tongue] [-W wrapcolumn] [message]\n",
      progname);
  exit(64);
}

/* ---------- cowfile parsing ----------
 *
 * A .cow file is a Perl script, but every included cowfile fits a small grammar:
 * comment and blank lines; the preamble statement idioms below;
 * one interpolating heredoc assigned to $the_cow; nothing after its terminator.
 * Anything else is rejected with an error naming the line, never mis-rendered.
 */

static const char *cow_source; // display name for error messages

static void cow_error(int lineno, const char *msg) {
  fprintf(stderr, "%s: %s:%d: %s\n", progname, cow_source, lineno, msg);
  exit(1);
}

/* Remove and return the last codepoint of *s (Perl chop, lifted to units). */
static char *chop_unit(char *s) {
  size_t n = strlen(s);
  if (n == 0) return xstrndup("", 0);
  size_t i = 0, last = 0;
  while (i < n) {
    last = i;
    i += u8_len_at(s, n, i);
  }
  char *out = xstrndup(s + last, n - last);
  s[last] = '\0';
  return out;
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

static int match_lit(const char **p, const char *lit) {
  size_t n = strlen(lit);
  if (strncmp(*p, lit, n) != 0) return 0;
  *p += n;
  return 1;
}

static int is_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

static char *match_ident(const char **p) {
  const char *s = *p;
  if (!is_ident_start(*s)) return NULL;
  while (is_ident(*s)) s++;
  char *id = xstrndup(*p, (size_t)(s - *p));
  *p = s;
  return id;
}

/* Scratch variables assigned in a cowfile preamble ($extra, $eye1, ...). */
static List cow_var_names;
static List cow_var_vals;

static void set_cow_var(char *name, char *val) {
  for (size_t i = 0; i < cow_var_names.n; i++)
    if (strcmp(cow_var_names.v[i], name) == 0) {
      free(cow_var_vals.v[i]);
      cow_var_vals.v[i] = val;
      free(name);
      return;
    }
  list_push(&cow_var_names, name);
  list_push(&cow_var_vals, val);
}

static const char *get_cow_var(const char *name) {
  for (size_t i = 0; i < cow_var_names.n; i++)
    if (strcmp(cow_var_names.v[i], name) == 0) return cow_var_vals.v[i];
  return NULL;
}

/* The value of $thoughts, $eyes, $tongue or a preamble variable, or NULL. */
static const char *var_value(const char *name) {
  if (strcmp(name, "thoughts") == 0) return thoughts;
  if (strcmp(name, "eyes") == 0) return eyes;
  if (strcmp(name, "tongue") == 0) return tongue;
  return get_cow_var(name);
}

static void interpolate_body_line(Buf *out, const char *line, size_t n, int lineno) {
  for (size_t i = 0; i < n;) {
    char c = line[i];
    if (c == '\\') {
      if (i + 1 >= n || line[i + 1] == '\n')
        cow_error(lineno, "unsupported backslash at end of line");
      char e = line[i + 1];
      if (e == '\\' || e == '$' || e == '@') {
        buf_push(out, e);
        i += 2;
      } else if (e == 'e') {
        buf_push(out, '\x1b'); // Perl "\e": clawd.cow's ANSI colors
        i += 2;
      } else {
        cow_error(lineno, "unsupported escape in cowfile");
      }
    } else if (c == '$') {
      const char *p = line + i + 1;
      char *name = NULL;
      if (*p == '{') {
        p++;
        name = match_ident(&p);
        if (!name || *p != '}') cow_error(lineno, "unsupported ${...} form");
        p++;
      } else {
        name = match_ident(&p);
      }
      if (!name) {
        buf_push(out, '$');
        i++;
        continue;
      }
      const char *val = var_value(name);
      if (!val) cow_error(lineno, "unknown variable in cowfile");
      buf_append(out, val, strlen(val));
      free(name);
      i = (size_t)(p - line);
    } else if (c == '@' && i + 1 < n && is_ident_start(line[i + 1])) {
      cow_error(lineno, "unescaped @ in cowfile");
    } else {
      buf_push(out, c);
      i++;
    }
  }
}

/* Parse one preamble statement; returns 1 if recognized.
 * Chopped characters land in the cow variable table, so a cowfile can chop more than once.
 */
static int parse_preamble_stmt(const char *line) {
  const char *p = skip_ws(line);
  // $<var> = chop($eyes);
  if (*p == '$') {
    const char *q = p + 1;
    char *name = match_ident(&q);
    if (name && strcmp(name, "the_cow") != 0 && strcmp(name, "eyes") != 0) {
      q = skip_ws(q);
      if (match_lit(&q, "=")) {
        q = skip_ws(q);
        if (match_lit(&q, "chop($eyes);")) {
          q = skip_ws(q);
          if (*q == '\0' || *q == '\n') {
            set_cow_var(name, chop_unit(eyes));
            return 1;
          }
        }
      }
    }
    free(name);
  }
  // $eyes .= ($<var> x 2);   and   $eyes .= " $<var>";
  const char *q = p;
  if (match_lit(&q, "$eyes")) {
    q = skip_ws(q);
    if (match_lit(&q, ".=")) {
      q = skip_ws(q);
      if (*q == '(' && q[1] == '$') {
        const char *r = q + 2;
        char *name = match_ident(&r);
        const char *v = name ? get_cow_var(name) : NULL;
        r = skip_ws(r);
        if (v && match_lit(&r, "x") && (r = skip_ws(r), match_lit(&r, "2);")) &&
          (r = skip_ws(r), *r == '\0' || *r == '\n')) {
          Buf b = {0};
          buf_append(&b, eyes, strlen(eyes));
          buf_append(&b, v, strlen(v));
          buf_append(&b, v, strlen(v));
          free(eyes);
          eyes = b.p;
          free(name);
          return 1;
        }
        free(name);
      } else if (*q == '"') {
        // $eyes .= " $<var>"; with one or more spaces (udder.cow uses one, clawd.cow two).
        const char *sp = q + 1;
        const char *r = sp;
        while (*r == ' ') r++;
        size_t nsp = (size_t)(r - sp);
        if (nsp > 0 && *r == '$') {
          r++;
          char *name = match_ident(&r);
          const char *v = name ? get_cow_var(name) : NULL;
          if (v && match_lit(&r, "\";") && (r = skip_ws(r), *r == '\0' || *r == '\n')) {
            Buf b = {0};
            buf_append(&b, eyes, strlen(eyes));
            buf_append(&b, sp, nsp);
            buf_append(&b, v, strlen(v));
            free(eyes);
            eyes = b.p;
            free(name);
            return 1;
          }
          free(name);
        }
      }
    }
    // $eyes = "<lit>" unless ($eyes);
    q = p;
    match_lit(&q, "$eyes");
    q = skip_ws(q);
    if (match_lit(&q, "=")) {
      q = skip_ws(q);
      if (*q == '"') {
        const char *r = strchr(q + 1, '"');
        if (r) {
          char *lit = xstrndup(q + 1, (size_t)(r - q - 1));
          const char *s2 = r + 1;
          s2 = skip_ws(s2);
          if (match_lit(&s2, "unless") && (s2 = skip_ws(s2), match_lit(&s2, "($eyes);")) &&
            (s2 = skip_ws(s2), *s2 == '\0' || *s2 == '\n')) {
            if (eyes[0] == '\0') {
              free(eyes);
              eyes = lit;
            } else {
              free(lit);
            }
            return 1;
          }
          free(lit);
        }
      }
    }
  }
  return 0;
}

/* Parse a cowfile's bytes into the cow text. */
static char *parse_cow(const char *data, size_t n) {
  Buf out = {0};
  char *term = NULL;
  size_t term_len = 0;
  enum { PREAMBLE, BODY, AFTER } state = PREAMBLE;
  int lineno = 0;
  size_t i = 0;
  while (i <= n) {
    if (i == n && state != BODY) break;
    if (i == n) cow_error(lineno, "unterminated heredoc");
    size_t eol = i;
    while (eol < n && data[eol] != '\n') eol++;
    size_t line_len = eol - i;              // without the newline
    size_t full_len = (eol < n) ? line_len + 1 : line_len;
    const char *line = data + i;
    lineno++;
    if (state == BODY) {
      if (line_len == term_len && strncmp(line, term, term_len) == 0) {
        state = AFTER;
      } else {
        interpolate_body_line(&out, line, full_len, lineno);
      }
    } else {
      const char *p = skip_ws(line);
      size_t rest = line_len - (size_t)(p - line);
      if (rest == 0 || *p == '#' || (rest == 1 && *p == '\r')) {
        // comment or blank line
      } else if (state == AFTER) {
        cow_error(lineno, "unsupported text after heredoc terminator");
      } else {
        const char *q = p;
        if (match_lit(&q, "$the_cow")) {
          q = skip_ws(q);
          if (!match_lit(&q, "=")) cow_error(lineno, "unsupported cowfile construct");
          q = skip_ws(q);
          if (!match_lit(&q, "<<")) cow_error(lineno, "unsupported cowfile construct");
          q = skip_ws(q);
          int quoted = (*q == '"');
          if (quoted) q++;
          char *t = match_ident(&q);
          if (!t) cow_error(lineno, "unsupported heredoc terminator");
          if (quoted && !match_lit(&q, "\"")) cow_error(lineno, "unsupported heredoc terminator");
          q = skip_ws(q);
          match_lit(&q, ";"); // sheep.cow omits the semicolon
          q = skip_ws(q);
          if (*q != '\0' && *q != '\n' && (size_t)(q - line) < line_len)
            cow_error(lineno, "unsupported cowfile construct");
          term = t;
          term_len = strlen(t);
          state = BODY;
        } else if (!parse_preamble_stmt(line)) {
          cow_error(lineno, "unsupported cowfile construct");
        }
      }
    }
    i += full_len;
    if (full_len == line_len) break; // last line had no newline
  }
  if (state == PREAMBLE) cow_error(lineno, "no $the_cow heredoc found");
  if (state == BODY) cow_error(lineno, "unterminated heredoc");
  free(term);
  if (!out.p) buf_append(&out, "", 0);
  return out.p;
}

/* ---------- cowfile lookup ---------- */

static int is_regular_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_file(const char *path, size_t *out_len) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  Buf b = {0};
  char chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0) buf_append(&b, chunk, got);
  int bad = ferror(fp);
  fclose(fp);
  if (bad) {
    free(b.p);
    return NULL;
  }
  if (!b.p) buf_append(&b, "", 0);
  *out_len = b.len;
  return b.p;
}

static const struct embedded_cow *find_embedded(const char *name) {
  for (size_t i = 0; i < sizeof embedded_cows / sizeof embedded_cows[0]; i++)
    if (strcmp(embedded_cows[i].name, name) == 0) return &embedded_cows[i];
  return NULL;
}

static char *get_cow(const char *f) {
  const char *cowpath = getenv("COWPATH");
  if (strchr(f, '/')) {
    // Deliberate fix: the reference runs `do $full` unchecked: a missing path exits 0 with no cow.
    // This reports it like any other missing cowfile.
    size_t n;
    char *data = read_file(f, &n);
    if (!data) {
      fprintf(stderr, "%s: Could not find %s cowfile!\n", progname, f);
      exit(2);
    }
    cow_source = f;
    char *cow = parse_cow(data, n);
    free(data);
    return cow;
  }
  if (cowpath && *cowpath) {
    const char *p = cowpath;
    while (1) {
      const char *colon = strchr(p, ':');
      size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
      for (int suffix = 0; suffix < 2; suffix++) {
        Buf path = {0};
        buf_append(&path, p, dlen);
        buf_push(&path, '/');
        buf_append(&path, f, strlen(f));
        if (suffix) buf_append(&path, ".cow", 4);
        if (is_regular_file(path.p)) {
          size_t n;
          char *data = read_file(path.p, &n);
          if (data) {
            cow_source = path.p;
            char *cow = parse_cow(data, n);
            free(data);
            free(path.p);
            return cow;
          }
        }
        free(path.p);
      }
      if (!colon) break;
      p = colon + 1;
    }
  } else {
    const struct embedded_cow *e = find_embedded(f);
    if (!e) {
      Buf name = {0};
      buf_append(&name, f, strlen(f));
      buf_append(&name, ".cow", 4);
      e = find_embedded(name.p);
      free(name.p);
    }
    if (e) {
      cow_source = e->name;
      return parse_cow((const char *)e->data, e->len);
    }
  }
  fprintf(stderr, "%s: Could not find %s cowfile!\n", progname, f);
  exit(2); // Perl die picks up $! = ENOENT from the failed file tests
}

/* ---------- -l listing ---------- */

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void print_name_list(List *names) {
  qsort(names->v, names->n, sizeof(char *), cmp_str);
  Buf joined = {0};
  for (size_t i = 0; i < names->n; i++) {
    if (i) buf_push(&joined, ' ');
    buf_append(&joined, names->v[i], strlen(names->v[i]));
  }
  // list_cowfiles runs before -W is applied, so this wraps at Text::Wrap's default of 76 columns.
  long cols = 76;
  char *w = wrap_text(joined.p ? joined.p : "", joined.len, &cols);
  fputs(w, stdout);
  fputs("\n", stdout);
  free(w);
  free(joined.p);
}

static void list_cowfiles(void) {
  const char *cowpath = getenv("COWPATH");
  if (cowpath && *cowpath) {
    const char *p = cowpath;
    while (1) {
      const char *colon = strchr(p, ':');
      size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
      char *dir = xstrndup(p, dlen);
      printf("Cow files in %s:\n", dir);
      DIR *dp = opendir(dir);
      if (!dp) {
        fprintf(stderr, "%s: Cannot open %s\n", argv0, dir);
        exit(2);
      }
      List names = {0};
      struct dirent *de;
      while ((de = readdir(dp)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcmp(de->d_name + len - 4, ".cow") == 0)
          list_push(&names, xstrndup(de->d_name, len - 4));
      }
      closedir(dp);
      print_name_list(&names);
      for (size_t i = 0; i < names.n; i++) free(names.v[i]);
      free(names.v);
      free(dir);
      if (!colon) break;
      p = colon + 1;
    }
  } else {
    printf("Cow files in (embedded):\n");
    List names = {0};
    for (size_t i = 0; i < sizeof embedded_cows / sizeof embedded_cows[0]; i++) {
      size_t len = strlen(embedded_cows[i].name);
      list_push(&names, xstrndup(embedded_cows[i].name, len - 4));
    }
    print_name_list(&names);
  }
  exit(0);
}

/* ---------- balloon ---------- */

static void emit_balloon_line(Buf *b, const char *bl, const char *s, size_t padw, const char *br) {
  buf_append(b, bl, strlen(bl));
  buf_push(b, ' ');
  buf_append(b, s, strlen(s));
  size_t units = u8_count(s, strlen(s));
  if (units < padw) buf_repeat(b, ' ', padw - units);
  buf_push(b, ' ');
  buf_append(b, br, strlen(br));
  buf_push(b, '\n');
}

static char *construct_balloon(char **lines, size_t n, int think) {
  long max = -1;
  for (size_t i = 0; i < n; i++) {
    long l = (long)u8_count(lines[i], strlen(lines[i]));
    if (l > max) max = l;
  }
  // max stays -1 for an empty message;
  // sprintf("%--1s", "") then pads to one column, and the border dashes span max + 2 = 1.
  size_t padw = (max < 0) ? 1 : (size_t)max;
  size_t max2 = (size_t)(max + 2);
  const char *b0, *b1, *b2, *b3, *b4, *b5;
  if (think) {
    thoughts = "o";
    b0 = "("; b1 = ")"; b2 = "("; b3 = ")"; b4 = "("; b5 = ")";
  } else if (n < 2) {
    thoughts = "\\";
    b0 = "<"; b1 = ">"; b2 = b3 = b4 = b5 = "";
  } else {
    thoughts = "\\";
    b0 = "/"; b1 = "\\"; b2 = "\\"; b3 = "/"; b4 = "|"; b5 = "|";
  }
  Buf b = {0};
  buf_push(&b, ' ');
  buf_repeat(&b, '_', max2);
  buf_append(&b, " \n", 2);
  emit_balloon_line(&b, b0, n ? lines[0] : "", padw, b1);
  if (n >= 2) {
    for (size_t i = 1; i + 1 < n; i++) emit_balloon_line(&b, b4, lines[i], padw, b5);
    emit_balloon_line(&b, b2, lines[n - 1], padw, b3);
  }
  buf_push(&b, ' ');
  buf_repeat(&b, '-', max2);
  buf_append(&b, " \n", 2);
  return b.p;
}

/* ---------- input ---------- */

static void read_stdin_lines(List *out) {
  Buf all = {0};
  char chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, stdin)) > 0) buf_append(&all, chunk, got);
  const char *s = all.p ? all.p : "";
  size_t n = all.len, start = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\n') {
      list_push(out, xstrndup(s + start, i - start)); // chomp
      start = i + 1;
    }
  }
  if (start < n) list_push(out, xstrndup(s + start, n - start));
  free(all.p);
}

static const char *basename_of(const char *path) {
  const char *b = path, *p = path;
  for (; *p; p++)
    if (*p == '/' && p[1]) b = p + 1;
  return b;
}

static int contains_think(const char *s) {
  // $0 =~ /think/i on the full invocation path
  for (; *s; s++) {
    const char *a = s, *b = "think";
    while (*b && ((*a | 0x20) == *b)) a++, b++;
    if (!*b) return 1;
  }
  return 0;
}

/* Perl numeric coercion of $opts{'W'}: optional whitespace and sign, then decimal digits.
 * Anything else contributes 0.
 * Fractional and exponent forms are outside the specification; see test/README.md.
 */
static long numify(const char *s) {
  while (is_space(*s)) s++;
  return strtol(s, NULL, 10);
}

int main(int argc, char **argv) {
  if (argc > 0 && argv[0] && argv[0][0]) argv0 = argv[0];
  progname = basename_of(argv0);
  int think = contains_think(argv0);
  thoughts = think ? "o" : "\\";

  Opts o = {0};
  o.e = "oo";
  o.f = "default.cow";
  o.T = "  ";
  o.W = "40";
  int rest = getopts(argc, argv, &o);

  if (o.h) display_usage();
  if (o.l) list_cowfiles();

  eyes = u8_prefix(o.e, 2);
  tongue = u8_prefix(o.T, 2);
  long columns = numify(o.W);

  // Deliberate fix: `unless ($ARGV[0])` makes a first argument of "" or "0" falsy in the reference;
  // it then waits on stdin, while here any remaining argument selects the argument message.
  List raw = {0};
  int use_args = rest < argc;
  if (use_args) {
    if (o.n) display_usage();
    Buf joined = {0};
    for (int i = rest; i < argc; i++) {
      if (i > rest) buf_push(&joined, ' ');
      buf_append(&joined, argv[i], strlen(argv[i]));
    }
    if (!joined.p) buf_append(&joined, "", 0);
    list_push(&raw, joined.p);
  } else {
    read_stdin_lines(&raw);
  }

  List message = {0};
  if (o.n) {
    for (size_t i = 0; i < raw.n; i++) list_push(&message, expand_line(raw.v[i]));
  } else {
    char *filled = fill_lines(raw.v, raw.n, &columns);
    // split("\n", ...): trailing empty fields are dropped
    size_t n = strlen(filled), end = n;
    while (end > 0 && filled[end - 1] == '\n') end--;
    if (end > 0) {
      size_t start = 0;
      for (size_t i = 0; i <= end; i++) {
        if (i == end || filled[i] == '\n') {
          list_push(&message, xstrndup(filled + start, i - start));
          start = i + 1;
        }
      }
    }
    free(filled);
  }

  char *balloon = construct_balloon(message.v, message.n, think);

  // construct_face: sequential ifs, so a later flag in this fixed order overrides an earlier one;
  // any of them overrides -e/-T.
  if (o.b) { free(eyes); eyes = xstrndup("==", 2); }
  if (o.d) { free(eyes); eyes = xstrndup("xx", 2); free(tongue); tongue = xstrndup("U ", 2); }
  if (o.g) { free(eyes); eyes = xstrndup("$$", 2); }
  if (o.p) { free(eyes); eyes = xstrndup("@@", 2); }
  if (o.s) { free(eyes); eyes = xstrndup("**", 2); free(tongue); tongue = xstrndup("U ", 2); }
  if (o.t) { free(eyes); eyes = xstrndup("--", 2); }
  if (o.w) { free(eyes); eyes = xstrndup("OO", 2); }
  if (o.y) { free(eyes); eyes = xstrndup("..", 2); }

  char *cow = get_cow(o.f);

  fputs(balloon, stdout);
  fputs(cow, stdout);
  return 0;
}
