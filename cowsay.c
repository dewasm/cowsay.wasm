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
#include "width.h"

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

/* The codepoint at `index`, as a fresh string, empty past the end: Perl substr($s, $i, 1). */
static char *u8_unit_at(const char *s, size_t index) {
  size_t n = strlen(s), i = 0;
  unsigned cp;
  while (index > 0 && i < n) {
    i += wu_decode(s, n, i, &cp);
    index--;
  }
  if (i >= n) return xstrndup("", 0);
  return xstrndup(s + i, wu_decode(s, n, i, &cp));
}

/* First `units` grapheme clusters of s: Perl substr($s, 0, 2) lifted to clusters. */
static char *cluster_prefix(const char *s, size_t units) {
  size_t n = strlen(s), i = 0;
  while (units > 0 && i < n) {
    i = wu_cluster_end(s, n, i);
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
    // Greedy match: the longest prefix of at most ll columns ending at a break or end of input.
    // A unit here is one grapheme cluster, so a break never lands inside one;
    // escape sequences ride along at no cost.
    size_t q = pos;
    long units = 0;
    size_t best = (size_t)-1;
    if (is_space(t[pos])) best = pos; // zero-column candidate
    while (units < ll && q < n && t[q] != '\n') {
      size_t esc = wu_escape_len(t, n, q);
      if (esc) {
        q += esc;
        continue;
      }
      size_t end = wu_cluster_end(t, n, q);
      int w = wu_cluster_width(t, n, q, end);
      if (units + w > ll) break;
      q = end;
      units += w ? w : 1; // a zero-width cluster still has to advance the count
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
    } else {
      // $huge = 'wrap': cut the overlong word where the columns run out.
      // A cluster wider than the line still goes out whole: not splitting one is the point.
      if (q == pos) q = wu_cluster_end(t, n, pos);
      if (!first) buf_push(&r, '\n');
      buf_append(&r, t + pos, q - pos);
      rem = NULL; // separator; never survives to the end of the loop
      pos = q;
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

/* Text::Tabs expand() with tabstop 8: a tab runs to the next multiple of eight columns.
 * Columns rather than characters, so a tab after a wide character still lands on its stop.
 */
static char *expand_line(const char *s) {
  Buf out = {0};
  size_t n = strlen(s), column = 0;
  for (size_t i = 0; i < n;) {
    if (s[i] == '\t') {
      size_t pad = 8 - column % 8;
      buf_repeat(&out, ' ', pad);
      column += pad;
      i++;
      continue;
    }
    size_t esc = wu_escape_len(s, n, i);
    if (esc) {
      buf_append(&out, s + i, esc);
      i += esc;
      continue;
    }
    size_t end = wu_cluster_end(s, n, i);
    buf_append(&out, s + i, end - i);
    column += (size_t)wu_cluster_width(s, n, i, end);
    i = end;
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

/* Remove and return the last codepoint of *s: Perl chop, lifted from bytes to codepoints. */
static char *chop_unit(char *s) {
  size_t n = strlen(s), i = 0, last = 0;
  unsigned cp;
  if (n == 0) return xstrndup("", 0);
  while (i < n) {
    last = i;
    i += wu_decode(s, n, i, &cp);
  }
  char *out = xstrndup(s + last, n - last);
  s[last] = '\0';
  return out;
}

// Whether a statement ends here: only the line's own newline may follow.
static int end_of_stmt(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return *p == '\0' || *p == '\n';
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

/* A double-quoted literal, as the statement idioms write it; no cowfile needs escapes there. */
static char *match_string(const char **p) {
  if (**p != '"') return NULL;
  const char *end = strchr(*p + 1, '"');
  if (!end) return NULL;
  char *lit = xstrndup(*p + 1, (size_t)(end - *p - 1));
  *p = end + 1;
  return lit;
}

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

/* Append to $eyes, which every append idiom writes to. */
static void eyes_append(const char *s, size_t n) {
  Buf b = {0};
  buf_append(&b, eyes, strlen(eyes));
  buf_append(&b, s, n);
  free(eyes);
  eyes = b.p;
}

/* Parse one preamble statement; returns 1 if recognized.
 * Chopped and extracted characters land in the cow variable table, so a cowfile can take several.
 */
static int parse_preamble_stmt(const char *line) {
  const char *p = skip_ws(line);
  if (*p != '$') return 0;

  // $<var> = chop($eyes);   and   $<var> = substr($eyes, <n>, 1);
  const char *q = p + 1;
  char *name = match_ident(&q);
  if (name && strcmp(name, "the_cow") != 0 && strcmp(name, "eyes") != 0) {
    q = skip_ws(q);
    if (match_lit(&q, "=")) {
      const char *r = skip_ws(q);
      if (match_lit(&r, "chop($eyes);") && end_of_stmt(r)) {
        set_cow_var(name, chop_unit(eyes));
        return 1;
      }
      r = skip_ws(q);
      if (match_lit(&r, "substr($eyes,")) {
        r = skip_ws(r);
        size_t at = 0, digits = 0;
        while (*r >= '0' && *r <= '9') {
          at = at * 10 + (size_t)(*r++ - '0');
          digits++;
        }
        r = skip_ws(r);
        if (digits && *r == ',') {
          r = skip_ws(r + 1);
          if (match_lit(&r, "1") && (r = skip_ws(r), match_lit(&r, ");")) && end_of_stmt(r)) {
            set_cow_var(name, u8_unit_at(eyes, at));
            return 1;
          }
        }
      }
    }
  }
  free(name);

  // Every remaining idiom assigns to $eyes.
  q = p;
  if (!match_lit(&q, "$eyes")) return 0;
  q = skip_ws(q);

  // $eyes .= ($<var> x 2);   $eyes .= " $<var>";   and   $eyes .= "<literal>";
  const char *tail = q;
  if (match_lit(&tail, ".=")) {
    const char *r = skip_ws(tail);
    if (*r == '(' && r[1] == '$') {
      const char *v_start = r + 2;
      char *var = match_ident(&v_start);
      const char *value = var ? get_cow_var(var) : NULL;
      v_start = skip_ws(v_start);
      if (value && match_lit(&v_start, "x") &&
        (v_start = skip_ws(v_start), match_lit(&v_start, "2);")) && end_of_stmt(v_start)) {
        eyes_append(value, strlen(value));
        eyes_append(value, strlen(value));
        free(var);
        return 1;
      }
      free(var);
    }
    r = skip_ws(tail);
    char *lit = match_string(&r);
    if (lit && match_lit(&r, ";") && end_of_stmt(r)) {
      // Spaces then a variable set above interpolate; a literal without one is appended as it is.
      const char *dollar = strchr(lit, '$');
      if (!dollar) {
        eyes_append(lit, strlen(lit));
        free(lit);
        return 1;
      }
      size_t spaces = (size_t)(dollar - lit);
      if (spaces > 0 && strspn(lit, " ") == spaces) {
        const char *v_start = dollar + 1;
        char *var = match_ident(&v_start);
        const char *value = var ? get_cow_var(var) : NULL;
        if (value && *v_start == '\0') {
          eyes_append(lit, spaces);
          eyes_append(value, strlen(value));
          free(var);
          free(lit);
          return 1;
        }
        free(var);
      }
    }
    free(lit);
  }

  // $eyes = "<literal>" unless ($eyes);   and   $eyes = "<literal>" if ($eyes eq "<literal>");
  tail = q;
  if (match_lit(&tail, "=")) {
    const char *r = skip_ws(tail);
    char *lit = match_string(&r);
    if (lit) {
      const char *cond = skip_ws(r);
      if (match_lit(&cond, "unless") && (cond = skip_ws(cond), match_lit(&cond, "($eyes);")) &&
        end_of_stmt(cond)) {
        if (eyes[0] == '\0') {
          free(eyes);
          eyes = lit;
        } else {
          free(lit);
        }
        return 1;
      }
      cond = skip_ws(r);
      if (match_lit(&cond, "if") && (cond = skip_ws(cond), match_lit(&cond, "($eyes eq"))) {
        cond = skip_ws(cond);
        char *want = match_string(&cond);
        if (want && (cond = skip_ws(cond), match_lit(&cond, ");")) && end_of_stmt(cond)) {
          if (strcmp(eyes, want) == 0) {
            free(eyes);
            eyes = lit;
          } else {
            free(lit);
          }
          free(want);
          return 1;
        }
        free(want);
      }
      free(lit);
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

/* The rendition a wrapped line inherits from the lines above it. */
static WuSgr balloon_sgr;

static void emit_balloon_line(Buf *b, const char *bl, const char *s, size_t padw, const char *br) {
  char open[96];
  wu_sgr_render(&balloon_sgr, open, sizeof open);
  buf_append(b, bl, strlen(bl));
  buf_push(b, ' ');
  // Reopening at the start and closing at the end keeps the colour inside the balloon text.
  // The frame and the padding then stay in the terminal's own colours.
  buf_append(b, open, strlen(open));
  buf_append(b, s, strlen(s));
  wu_sgr_scan(&balloon_sgr, s, strlen(s));
  if (wu_sgr_active(&balloon_sgr)) buf_append(b, "\033[0m", 4);
  size_t units = wu_display_width(s, strlen(s));
  if (units < padw) buf_repeat(b, ' ', padw - units);
  buf_push(b, ' ');
  buf_append(b, br, strlen(br));
  buf_push(b, '\n');
}

static char *construct_balloon(char **lines, size_t n, int think) {
  wu_sgr_clear(&balloon_sgr);
  long max = -1;
  for (size_t i = 0; i < n; i++) {
    long l = (long)wu_display_width(lines[i], strlen(lines[i]));
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

/* East Asian Ambiguous characters take one column, per Unicode's default.
 * A CJK locale makes them two, as terminals and older libc implementations do, and
 * COWSAY_AMBIGUOUS_WIDTH=1 or =2 settles it outright.
 * A wasm runtime passes no environment unless asked, so the default holds until one arrives. */
static int ambiguous_is_wide(void) {
  const char *override = getenv("COWSAY_AMBIGUOUS_WIDTH");
  if (override && *override) return *override == '2';
  const char *locale = getenv("LC_ALL");
  if (!locale || !*locale) locale = getenv("LC_CTYPE");
  if (!locale || !*locale) locale = getenv("LANG");
  if (!locale) return 0;
  static const char *cjk[] = {"ja", "ko", "zh"};
  for (size_t i = 0; i < sizeof cjk / sizeof cjk[0]; i++) {
    size_t len = strlen(cjk[i]);
    if (strncmp(locale, cjk[i], len) != 0) continue;
    if (locale[len] == '\0' || locale[len] == '_' || locale[len] == '.') return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  wu_set_ambiguous_wide(ambiguous_is_wide());
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

  eyes = cluster_prefix(o.e, 2);
  tongue = cluster_prefix(o.T, 2);
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
