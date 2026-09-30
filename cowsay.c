/*
 * cowsay.wasm: a C reimplementation of cowsay 3.8.4, for wasm32-wasip1.
 * cowsay is (c) 1999-2000 Tony Monroe, and cowsay-org maintains it since.
 *
 * The output specification and the intended fixes are documented in test/README.md.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// arc4random_uniform is outside ISO C, which -std=c99 would keep to.
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cows_embedded.h"
#include "width.h"

/* The cowsay this reimplements, and the version of this reimplementation; -h names both. */
#define COWSAY_VERSION "3.8.4"
#define COWSAY_WASM_VERSION "0.2.0"

/* ---------- output ----------
 *
 * Output goes to write() rather than stdio: printf and FILE would add about 17 kB to the binary.
 */

/* Write each string argument in turn, up to a NULL. */
static void say(int fd, ...) {
  va_list ap;
  va_start(ap, fd);
  for (const char *s; (s = va_arg(ap, const char *)) != NULL;) {
    for (size_t n = strlen(s); n;) {
      ssize_t w = write(fd, s, n);
      if (w <= 0) break;
      s += w;
      n -= (size_t)w;
    }
  }
  va_end(ap);
}

/* ---------- growable byte buffer and string list ---------- */

typedef struct {
  char *p;
  size_t len, cap;
} Buf;

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) {
    say(STDERR_FILENO, "cowsay: out of memory\n", NULL);
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
  // Intended fix: below two columns the reference feeds Text::Wrap a negative regex quantifier;
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

/* ---------- help ---------- */

static const char *progname = "cowsay";

#define VERSION_LINE "version " COWSAY_VERSION " (cowsay.wasm " COWSAY_WASM_VERSION ")"

/* Intended fix: the version line also names this build, beside the cowsay it implements.
 * -C is left out, since this build does not have it.
 */
static void display_help(int status) {
  say(STDOUT_FILENO, progname,
      " " VERSION_LINE "\n"
      "\n"
      "Usage:\n"
      "\n"
      "    ",
      progname,
      " [-bdgpstwy] [-f <cowfile>] [-r] [-e <eyes>] [-T <tongue>]\n"
      "        [-W <wrapcolumn>] [-n]\n"
      "        <message>\n"
      "\n"
      "    ",
      progname,
      " -l              # List defined cows\n"
      "    ",
      progname,
      " [-h | --help]   # Display this help screen\n"
      "\n"
      "Options:\n"
      "\n"
      "    -b, -d, -g, -s, -t, -w, and -y activate Borg, dead, greedy, sleepy, tired, "
      "wired, and\n"
      "        young appearance modes, respectively.\n"
      "\n"
      "    -f <cowfile> selects an alternate cow picture. "
      "<cowfile> may be either the name of a\n"
      "        cow defined in a cowdir on the cowpath "
      "(without the '.cow' file extension), or the\n"
      "        path to a cowfile (with the '.cow' file extension). `",
      progname,
      " -l` will list the\n"
      "        names of available cows.\n"
      "\n"
      "    -r selects a random cowfile from those present on the cowpath.\n"
      "\n"
      "    -e <eyes> defines a custom eye appearance. <eyes> should be a two-character string.\n"
      "        It is up to you whether they actually look like eyes.\n"
      "\n"
      "    -T <tongue> defines a custom tongue appearance.\n"
      "\n"
      "    -n activates word wrapping, to support messages with arbitrary whitespace. "
      "Must be the\n"
      "        last option given before <message> starts.\n"
      "\n"
      "    -W <wrapcolumn> controls where line wrapping occurs. Default is 40 columns.\n"
      "\n",
      NULL);
  exit(status);
}

static void display_version(void) {
  say(STDOUT_FILENO, progname, " " VERSION_LINE "\n", NULL);
  exit(0);
}

/* ---------- option parsing: Getopt::Std getopts() port ---------- */

static const char OPTSTRING[] = "bde:f:ghlLnNprstT:wW:y";

typedef struct {
  const char *e, *f, *T, *W;
  int b, d, g, p, s, t, w, y, h, l, n, r;
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
    // Getopt::Std with $STANDARD_HELP_VERSION; its output also names the Perl version.
    if (strcmp(cluster, "-help") == 0) display_help(0);
    if (strcmp(cluster, "-version") == 0) display_version();
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
          case 'r': o->r = 1; break;
          default: break; // L, N: accepted and ignored, as upstream
        }
        cluster = *rest ? rest : NULL;
      }
    } else {
      char opt[2] = {c, '\0'};
      say(STDERR_FILENO, "Unknown option: ", opt, "\n", NULL);
      cluster = *rest ? rest : NULL;
    }
  }
  return ai;
}

/* ---------- globals mirroring the Perl script ---------- */

static const char *argv0 = "cowsay";
static char *eyes;
static char *tongue;
static const char *thoughts;


/* ---------- cowfile parsing ----------
 *
 * A .cow file is a Perl script, but every included cowfile fits a small grammar:
 * comment and blank lines; the preamble statement idioms below;
 * one interpolating heredoc assigned to $the_cow; nothing after its terminator.
 * Anything else is rejected with an error naming the line, never mis-rendered.
 */

static const char *cow_source; // display name for error messages

static void cow_error(int lineno, const char *msg) {
  char digits[16], *d = digits + sizeof digits;
  *--d = '\0';
  do *--d = (char)('0' + lineno % 10); while (lineno /= 10);
  say(STDERR_FILENO, progname, ": ", cow_source, ":", d, ": ", msg, "\n", NULL);
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

// Whether a statement ends here: only a comment or the line's own newline may follow.
static int end_of_stmt(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return *p == '\0' || *p == '\n' || *p == '#';
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

/* What a Perl string holds beyond ASCII, which decides the bytes Perl prints for it.
 * Perl prints a string with a character above U+00FF as UTF-8, and one without as a byte each.
 */
typedef struct {
  int wide;        // a character above U+00FF
  int latin1_line; // the first line of a character from U+0080 to U+00FF, or 0
  int byte_line;   // the first line of a raw byte above 0x7F, or 0
} Enc;

static void enc_merge(Enc *into, const Enc *from) {
  into->wide |= from->wide;
  if (!into->latin1_line) into->latin1_line = from->latin1_line;
  if (!into->byte_line) into->byte_line = from->byte_line;
}

static int enc_plain(const Enc *e) { return !e->wide && !e->latin1_line && !e->byte_line; }

/* The parts of Enc that decide the output are never guessed: a cow either prints as Perl
 * prints it, or is refused here.
 */
static void enc_check(const Enc *e) {
  // Perl would encode each raw byte again as a Latin-1 character.
  if (e->wide && e->byte_line) cow_error(e->byte_line, "unsupported byte beside a wide character");
  // Perl would print a lone byte, which is no UTF-8.
  if (!e->wide && e->latin1_line) cow_error(e->latin1_line, "unsupported character U+0080-U+00FF");
}

/* A scalar that a cowfile preamble sets ($extra, $eye1, $x, ...). */
typedef struct {
  char *name;
  char *val;
  Enc enc;
} Var;

static Var *cow_vars;
static size_t cow_var_count, cow_var_cap;

static Var *find_cow_var(const char *name) {
  for (size_t i = 0; i < cow_var_count; i++)
    if (strcmp(cow_vars[i].name, name) == 0) return &cow_vars[i];
  return NULL;
}

// $eyes, $tongue and $thoughts start from the command line, as text with nothing to encode.
static Enc eyes_enc, tongue_enc, thoughts_enc;

/* Assign to $thoughts, $eyes, $tongue or a preamble variable; takes ownership of val. */
static void set_var(const char *name, char *val, Enc enc) {
  if (strcmp(name, "thoughts") == 0) {
    thoughts = val;
    thoughts_enc = enc;
  } else if (strcmp(name, "eyes") == 0) {
    free(eyes);
    eyes = val;
    eyes_enc = enc;
  } else if (strcmp(name, "tongue") == 0) {
    free(tongue);
    tongue = val;
    tongue_enc = enc;
  } else {
    Var *v = find_cow_var(name);
    if (!v) {
      if (cow_var_count == cow_var_cap) {
        cow_var_cap = cow_var_cap ? cow_var_cap * 2 : 8;
        cow_vars = xrealloc(cow_vars, cow_var_cap * sizeof *cow_vars);
      }
      v = &cow_vars[cow_var_count++];
      v->name = xstrndup(name, strlen(name));
      v->val = NULL;
    }
    free(v->val);
    v->val = val;
    v->enc = enc;
  }
}

/* The value of $thoughts, $eyes, $tongue or a preamble variable, or NULL. */
static const char *var_value(const char *name, Enc *enc) {
  if (strcmp(name, "thoughts") == 0) return *enc = thoughts_enc, thoughts;
  if (strcmp(name, "eyes") == 0) return *enc = eyes_enc, eyes;
  if (strcmp(name, "tongue") == 0) return *enc = tongue_enc, tongue;
  Var *v = find_cow_var(name);
  if (!v) return NULL;
  *enc = v->enc;
  return v->val;
}

/* Whether Perl reads what follows an unbraced $name as an element or a package variable. */
static int subscript_follows(const char *p) {
  if (*p == '[' || *p == '{') return 1;
  if (p[0] == '-' && p[1] == '>') return p[2] == '[' || p[2] == '{';
  if (p[0] == ':' && p[1] == ':') return 1;
  return p[0] == '\'' && is_ident_start(p[1]);
}

/* Whether Perl interpolates an array at an @ followed by c; toke.c scan_const lists these. */
static int at_interpolates(char c) {
  return is_ident(c) || (c != '\0' && strchr(":'{$+-", c) != NULL);
}

/* Append a character that an escape names, as UTF-8. */
static void push_char(Buf *out, Enc *enc, unsigned long cp, int lineno) {
  // A NUL would end the string here; a surrogate or a value past Unicode is no character.
  if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
    cow_error(lineno, "unsupported escape in cowfile");
  if (cp < 0x80) {
    buf_push(out, (char)cp);
    return;
  }
  if (cp <= 0xFF) {
    if (!enc->latin1_line) enc->latin1_line = lineno;
  } else {
    enc->wide = 1;
  }
  char u[4];
  size_t len;
  if (cp < 0x800) {
    u[0] = (char)(0xC0 | (cp >> 6));
    len = 2;
  } else if (cp < 0x10000) {
    u[0] = (char)(0xE0 | (cp >> 12));
    u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    len = 3;
  } else {
    u[0] = (char)(0xF0 | (cp >> 18));
    u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    len = 4;
  }
  u[len - 1] = (char)(0x80 | (cp & 0x3F));
  buf_append(out, u, len);
}

/* Digits of a base up to max of them; returns how many it read, 0 if an overflow. */
static size_t read_digits(const char *s, size_t n, int base, size_t max, unsigned long *value) {
  size_t i = 0;
  *value = 0;
  while (i < n && i < max) {
    int d = -1;
    char c = s[i];
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    if (d < 0 || d >= base) break;
    *value = *value * (unsigned long)base + (unsigned long)d;
    if (*value > 0x10FFFF) return 0;
    i++;
  }
  return i;
}

/* A braced number such as {263A}; returns its length, 0 if it is not one. */
static size_t read_braced(const char *s, size_t n, int base, unsigned long *value) {
  if (n < 2 || s[0] != '{') return 0;
  size_t digits = read_digits(s + 1, n - 1, base, n - 1, value);
  if (digits == 0 || digits + 1 >= n || s[digits + 1] != '}') return 0;
  return digits + 2;
}

// Set by `use utf8;`: Perl then reads the rest of the source as UTF-8 characters, not bytes.
static int source_utf8;

/* Append one character of the source as it is; returns its length in bytes. */
static size_t push_source(Buf *out, Enc *enc, const char *s, size_t n, int lineno) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) {
    buf_push(out, (char)c);
    return 1;
  }
  if (!source_utf8) {
    if (!enc->byte_line) enc->byte_line = lineno;
    buf_push(out, (char)c);
    return 1;
  }
  unsigned cp;
  size_t len = wu_decode(s, n, 0, &cp);
  if (len == 1) cow_error(lineno, "malformed UTF-8 in cowfile");
  if (cp > 0xFF) {
    enc->wide = 1;
  } else if (!enc->latin1_line) {
    enc->latin1_line = lineno;
  }
  buf_append(out, s, len);
  return len;
}

/* The escape after a backslash, as perlop lists them for a double-quoted string.
 * Returns its length after the backslash.
 */
static size_t read_escape(Buf *out, Enc *enc, const char *s, size_t n, int lineno) {
  unsigned long cp;
  size_t len;
  char e = s[0];
  switch (e) {
  case 't': buf_push(out, '\t'); return 1;
  case 'n': buf_push(out, '\n'); return 1;
  case 'r': buf_push(out, '\r'); return 1;
  case 'f': buf_push(out, '\f'); return 1;
  case 'b': buf_push(out, '\b'); return 1;
  case 'a': buf_push(out, '\a'); return 1;
  case 'e': buf_push(out, '\x1b'); return 1;
  case 'c':
    if (n < 2) break;
    if (s[1] == '?') {
      buf_push(out, 0x7F);
      return 2;
    }
    char x = (s[1] >= 'a' && s[1] <= 'z') ? (char)(s[1] - 'a' + 'A') : s[1];
    if (x <= '@' || x > '_') break; // \c@ is a NUL
    buf_push(out, (char)(x ^ 0x40));
    return 2;
  case 'x':
    if ((len = read_braced(s + 1, n - 1, 16, &cp))) {
      push_char(out, enc, cp, lineno);
      return len + 1;
    }
    if (n > 1 && s[1] == '{') break;
    len = read_digits(s + 1, n - 1, 16, 2, &cp);
    push_char(out, enc, cp, lineno);
    return len + 1;
  case 'N':
    if (n < 4 || s[1] != '{' || s[2] != 'U' || s[3] != '+') break;
    len = read_digits(s + 4, n - 4, 16, n - 4, &cp);
    if (len == 0 || 4 + len >= n || s[4 + len] != '}') break;
    push_char(out, enc, cp, lineno);
    return len + 5;
  case 'o':
    if (!(len = read_braced(s + 1, n - 1, 8, &cp))) break;
    push_char(out, enc, cp, lineno);
    return len + 1;
  case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
    len = read_digits(s, n, 8, 3, &cp);
    push_char(out, enc, cp, lineno);
    return len;
  case 'u': case 'l': case 'U': case 'L': case 'Q': case 'E': case 'F':
    break; // these change the case of what follows
  default:
    // Any other character stands for itself, a letter as much as a newline.
    return push_source(out, enc, s, n, lineno);
  }
  cow_error(lineno, "unsupported escape in cowfile");
  return 0;
}

/* Interpolate as Perl does a double-quoted string, refusing whatever the grammar leaves out. */
static void interpolate(Buf *out, Enc *enc, const char *line, size_t n, int lineno) {
  for (size_t i = 0; i < n;) {
    char c = line[i];
    if (c == '\\') {
      if (i + 1 >= n) cow_error(lineno, "unsupported backslash at end of file");
      i += 1 + read_escape(out, enc, line + i + 1, n - i - 1, lineno);
    } else if (c == '$') {
      const char *p = line + i + 1;
      char *name = NULL;
      if (*p == '{') {
        p++;
        name = match_ident(&p);
        if (!name || *p != '}') cow_error(lineno, "unsupported ${...} form");
        p++;
      } else {
        // Perl reads a special variable here, or skips spaces to find a name.
        name = match_ident(&p);
        if (!name) cow_error(lineno, "unescaped $ in cowfile");
        if (subscript_follows(p)) cow_error(lineno, "unsupported subscript in cowfile");
      }
      Enc val_enc;
      const char *val = var_value(name, &val_enc);
      if (!val) cow_error(lineno, "unknown variable in cowfile");
      buf_append(out, val, strlen(val));
      enc_merge(enc, &val_enc);
      free(name);
      i = (size_t)(p - line);
    } else if (c == '@' && i + 1 < n && at_interpolates(line[i + 1])) {
      cow_error(lineno, "unescaped @ in cowfile");
    } else {
      i += push_source(out, enc, line + i, n - i, lineno);
    }
  }
}

/* A single-quoted heredoc: no interpolation, and a backslash is text too. */
static void append_literal(Buf *out, Enc *enc, const char *line, size_t n, int lineno) {
  for (size_t i = 0; i < n;) i += push_source(out, enc, line + i, n - i, lineno);
}

/* A literal on one line, '...' or an interpolated "..."; NULL if none starts at *p. */
static char *match_string(const char **p, Enc *enc, int lineno) {
  char quote = **p;
  if (quote != '"' && quote != '\'') return NULL;
  const char *start = *p + 1, *end = start;
  while (*end != '\0' && *end != '\n' && *end != quote)
    end += (end[0] == '\\' && end[1] != '\0' && end[1] != '\n') ? 2 : 1;
  if (*end != quote) return NULL;
  Buf b = {0};
  buf_append(&b, "", 0);
  *enc = (Enc){0};
  if (quote == '"') {
    interpolate(&b, enc, start, (size_t)(end - start), lineno);
  } else {
    // Only \\ and \' are escapes in single quotes.
    for (const char *q = start; q < end;) {
      if (q[0] == '\\' && (q[1] == '\\' || q[1] == '\'')) q++;
      q += push_source(&b, enc, q, (size_t)(end - q), lineno);
    }
  }
  *p = end + 1;
  return b.p;
}

/* The rest of `$<name> .= ...`: ($<var> x 2); or a literal. */
static int parse_append(const char *name, const char *r, int lineno) {
  Enc enc, more;
  const char *old = var_value(name, &enc);
  if (!old) return 0;
  Buf b = {0};
  buf_append(&b, old, strlen(old));
  if (match_lit(&r, "($")) {
    char *var = match_ident(&r);
    const char *value = var ? var_value(var, &more) : NULL;
    free(var);
    r = skip_ws(r);
    if (!value || !match_lit(&r, "x")) goto refuse;
    r = skip_ws(r);
    if (!match_lit(&r, "2);") || !end_of_stmt(r)) goto refuse;
    buf_append(&b, value, strlen(value));
    buf_append(&b, value, strlen(value));
  } else {
    char *lit = match_string(&r, &more, lineno);
    if (!lit) goto refuse;
    buf_append(&b, lit, strlen(lit));
    free(lit);
    if (!match_lit(&r, ";") || !end_of_stmt(r)) goto refuse;
  }
  enc_merge(&enc, &more);
  set_var(name, b.p, enc);
  return 1;
refuse:
  free(b.p);
  return 0;
}

/* A condition on $eyes: ($eyes), ($eyes eq "..."), or ($eyes ne "...").
 * Returns whether it holds, or -1 if none starts at *p.
 */
static int match_condition(const char **p, int lineno) {
  const char *r = skip_ws(*p);
  if (!match_lit(&r, "(")) return -1;
  r = skip_ws(r);
  if (!match_lit(&r, "$eyes")) return -1;
  r = skip_ws(r);
  int holds;
  if (match_lit(&r, ")")) {
    // Intended fix: Perl takes "0" as false too, which drops the eye that `-e 0` asks for.
    holds = eyes[0] != '\0';
  } else {
    int negated = match_lit(&r, "ne");
    if (!negated && !match_lit(&r, "eq")) return -1;
    r = skip_ws(r);
    Enc want_enc;
    char *want = match_string(&r, &want_enc, lineno);
    if (!want) return -1;
    // Equal bytes are equal characters only in plain text.
    int plain = enc_plain(&eyes_enc) && enc_plain(&want_enc);
    holds = (strcmp(eyes, want) == 0) != negated;
    free(want);
    r = skip_ws(r);
    if (!plain || !match_lit(&r, ")")) return -1;
  }
  *p = r;
  return holds;
}

/* The rest of `$<name> = ...`: chop($eyes); substr($eyes, <n>, 1); or a literal,
 * which for $eyes may carry `if` or `unless` and a condition.
 * chop, substr and eq count codepoints and compare bytes, which holds for plain text only.
 */
static int parse_assign(const char *name, const char *r, int lineno) {
  int to_eyes = strcmp(name, "eyes") == 0;
  if (!to_eyes && enc_plain(&eyes_enc) && match_lit(&r, "chop($eyes);") && end_of_stmt(r)) {
    set_var(name, chop_unit(eyes), eyes_enc);
    return 1;
  }
  if (!to_eyes && enc_plain(&eyes_enc) && match_lit(&r, "substr($eyes,")) {
    r = skip_ws(r);
    size_t at = 0, digits = 0;
    while (*r >= '0' && *r <= '9') {
      at = at * 10 + (size_t)(*r++ - '0');
      digits++;
    }
    r = skip_ws(r);
    if (!digits || !match_lit(&r, ",")) return 0;
    r = skip_ws(r);
    if (!match_lit(&r, "1")) return 0;
    r = skip_ws(r);
    if (!match_lit(&r, ");") || !end_of_stmt(r)) return 0;
    set_var(name, u8_unit_at(eyes, at), eyes_enc);
    return 1;
  }
  Enc enc;
  char *lit = match_string(&r, &enc, lineno);
  if (!lit) return 0;
  r = skip_ws(r);
  int take = 1;
  if (to_eyes) {
    int unless = match_lit(&r, "unless");
    if (unless || match_lit(&r, "if")) {
      int holds = match_condition(&r, lineno);
      if (holds < 0) goto refuse;
      take = holds != unless;
    }
  }
  if (!match_lit(&r, ";") || !end_of_stmt(r)) goto refuse;
  if (take) {
    set_var(name, lit, enc);
  } else {
    free(lit);
  }
  return 1;
refuse:
  free(lit);
  return 0;
}

/* Parse one preamble statement: `use utf8;`, `$<name> = ...` or `$<name> .= ...`.
 * Returns 1 if recognized.
 */
static int parse_preamble_stmt(const char *line, int lineno) {
  const char *p = skip_ws(line);
  if (match_lit(&p, "use")) {
    const char *q = skip_ws(p);
    if (q == p || !match_lit(&q, "utf8")) return 0;
    q = skip_ws(q);
    if (!match_lit(&q, ";") || !end_of_stmt(q)) return 0;
    source_utf8 = 1;
    return 1;
  }
  if (!match_lit(&p, "$")) return 0;
  char *name = match_ident(&p);
  if (!name || strcmp(name, "the_cow") == 0) {
    free(name);
    return 0;
  }
  p = skip_ws(p);
  int ok = 0;
  if (match_lit(&p, ".=")) {
    ok = parse_append(name, skip_ws(p), lineno);
  } else if (match_lit(&p, "=")) {
    ok = parse_assign(name, skip_ws(p), lineno);
  }
  free(name);
  return ok;
}

/* Parse a cowfile's bytes into the cow text. */
static char *parse_cow(const char *raw, size_t raw_len) {
  // Perl drops a CR before an LF anywhere in its source, as a build without PERL_STRICT_CR does.
  Buf src = {0};
  buf_append(&src, "", 0);
  for (size_t k = 0; k < raw_len; k++)
    if (!(raw[k] == '\r' && k + 1 < raw_len && raw[k + 1] == '\n')) buf_push(&src, raw[k]);
  const char *data = src.p;
  size_t n = src.len;
  Buf out = {0};
  Enc enc = {0};
  char *term = NULL;
  size_t term_len = 0;
  enum { PREAMBLE, BODY, AFTER } state = PREAMBLE;
  int literal = 0; // a heredoc under a single-quoted terminator
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
        if (literal) {
          append_literal(&out, &enc, line, full_len, lineno);
        } else {
          interpolate(&out, &enc, line, full_len, lineno);
        }
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
          // Perl allows a space after << only before a quoted terminator.
          const char *after = skip_ws(q);
          char quote = (*after == '"' || *after == '\'') ? *after : '\0';
          if (quote) q = after + 1;
          char *t = match_ident(&q);
          if (!t) cow_error(lineno, "unsupported heredoc terminator");
          if (quote && *q++ != quote) cow_error(lineno, "unsupported heredoc terminator");
          literal = quote == '\'';
          q = skip_ws(q);
          match_lit(&q, ";"); // sheep.cow omits the semicolon
          if (!end_of_stmt(q)) cow_error(lineno, "unsupported cowfile construct");
          term = t;
          term_len = strlen(t);
          state = BODY;
        } else if (!parse_preamble_stmt(line, lineno)) {
          cow_error(lineno, "unsupported cowfile construct");
        }
      }
    }
    i += full_len;
    if (full_len == line_len) break; // last line had no newline
  }
  if (state == PREAMBLE) cow_error(lineno, "no $the_cow heredoc found");
  if (state == BODY) cow_error(lineno, "unterminated heredoc");
  enc_check(&enc);
  free(term);
  free(src.p);
  if (!out.p) buf_append(&out, "", 0);
  return out.p;
}

/* ---------- cowfile lookup ---------- */

static int is_regular_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_file(const char *path, size_t *out_len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  Buf b = {0};
  char chunk[4096];
  ssize_t got;
  while ((got = read(fd, chunk, sizeof chunk)) > 0) buf_append(&b, chunk, (size_t)got);
  close(fd);
  int bad = got < 0;
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

/* ---------- cowpath ---------- */

// The built-in cowfiles stand first in the cowpath, where the reference has its own.
static const char builtin_dir[] = "(embedded)";

/* Perl's `$s == 1` for the decimal number at the start of s, after whitespace.
 * The digits are compared as written, since strtod would pull a float parser into the build.
 */
static int perl_num_is_one(const char *s) {
  while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
  if (*s == '+') s++;
  // The significant digits, and the power of ten of the first one.
  const char *first = NULL, *last = NULL;
  long power = -1;
  int before_point = 1;
  for (; (*s >= '0' && *s <= '9') || (*s == '.' && before_point); s++) {
    if (*s == '.') {
      before_point = 0;
      continue;
    }
    if (!first && *s == '0') {
      if (!before_point) power--;
      continue;
    }
    if (!first) first = s;
    if (*s != '0') last = s;
    if (before_point) power++;
  }
  if (!first) return 0;
  if (*s == 'e' || *s == 'E') {
    const char *q = s + 1;
    int negative = *q == '-';
    if (*q == '+' || *q == '-') q++;
    long exponent = 0;
    for (; *q >= '0' && *q <= '9' && exponent < 100000; q++) exponent = exponent * 10 + (*q - '0');
    power += negative ? -exponent : exponent;
  }
  // 1 is the one significant digit 1, in the ones place.
  return first == last && *first == '1' && power == 0;
}

/* The directories to search for a cowfile, in order and without repeats.
 * COWPATH comes after the built-in cowfiles, or alone under COWSAY_ONLY_COWPATH=1.
 * Perl's split on ':' keeps a leading empty field and drops trailing empty fields.
 */
static List cowpath_dirs(void) {
  List dirs = {0};
  const char *cowpath = getenv("COWPATH");
  const char *only = getenv("COWSAY_ONLY_COWPATH");
  // `if ($ENV{'COWPATH'})`: "" and "0" are false.
  int has_cowpath = cowpath && *cowpath && strcmp(cowpath, "0") != 0;
  if (!(has_cowpath && only && perl_num_is_one(only))) list_push(&dirs, (char *)builtin_dir);
  if (!has_cowpath) return dirs;
  size_t len = strlen(cowpath);
  while (len > 0 && cowpath[len - 1] == ':') len--;
  for (size_t start = 0; start <= len;) {
    size_t end = start;
    while (end < len && cowpath[end] != ':') end++;
    char *dir = xstrndup(cowpath + start, end - start);
    int seen = 0;
    for (size_t i = 0; i < dirs.n; i++)
      seen |= dirs.v[i] != builtin_dir && strcmp(dirs.v[i], dir) == 0;
    if (seen) {
      free(dir);
    } else {
      list_push(&dirs, dir);
    }
    start = end + 1;
  }
  return dirs;
}

static char *load_cow_file(const char *path) {
  size_t n;
  char *data = read_file(path, &n);
  if (!data) return NULL;
  cow_source = path;
  char *cow = parse_cow(data, n);
  free(data);
  return cow;
}

/* The cow of name in dir: the file name itself, then name.cow, as the reference tries them. */
static char *cow_in_dir(const char *dir, const char *name) {
  if (dir == builtin_dir) {
    const struct embedded_cow *e = find_embedded(name);
    if (!e) {
      Buf with_suffix = {0};
      buf_append(&with_suffix, name, strlen(name));
      buf_append(&with_suffix, ".cow", 4);
      e = find_embedded(with_suffix.p);
      free(with_suffix.p);
    }
    if (!e) return NULL;
    cow_source = e->name;
    return parse_cow((const char *)e->data, e->len);
  }
  for (int suffix = 0; suffix < 2; suffix++) {
    Buf path = {0};
    buf_append(&path, dir, strlen(dir));
    buf_push(&path, '/');
    buf_append(&path, name, strlen(name));
    if (suffix) buf_append(&path, ".cow", 4);
    char *cow = is_regular_file(path.p) ? load_cow_file(path.p) : NULL;
    if (cow) return cow; // cow_source keeps path.p
    free(path.p);
  }
  return NULL;
}

static char *cow_from_cowpath(const char *name) {
  List dirs = cowpath_dirs();
  for (size_t i = 0; i < dirs.n; i++) {
    char *cow = cow_in_dir(dirs.v[i], name);
    if (cow) return cow;
  }
  say(STDERR_FILENO, progname, ": Could not find cowfile for '", name, "'!\n", NULL);
  exit(2); // Perl die picks up $! = ENOENT from the failed file tests
}

static char *get_cow(const char *f) {
  // A file of that name comes first, as in the reference.
  // Intended fix: the reference loads it with `do`, which searches @INC for a relative path
  // that starts with neither ./ nor ../, and so prints no cow; this reads the file.
  if (is_regular_file(f)) {
    char *cow = load_cow_file(f);
    if (cow) return cow;
  }
  return cow_from_cowpath(f);
}

/* ---------- -l listing ---------- */

/* Insertion sort: the lists are cowfile names, and qsort would add about 2 kB to the binary. */
static void sort_names(char **v, size_t n) {
  for (size_t i = 1; i < n; i++) {
    char *name = v[i];
    size_t k = i;
    for (; k > 0 && strcmp(v[k - 1], name) > 0; k--) v[k] = v[k - 1];
    v[k] = name;
  }
}

static void print_name_list(List *names) {
  sort_names(names->v, names->n);
  Buf joined = {0};
  for (size_t i = 0; i < names->n; i++) {
    if (i) buf_push(&joined, ' ');
    buf_append(&joined, names->v[i], strlen(names->v[i]));
  }
  // list_cowfiles runs before -W is applied, so this wraps at Text::Wrap's default of 76 columns.
  long cols = 76;
  char *w = wrap_text(joined.p ? joined.p : "", joined.len, &cols);
  say(STDOUT_FILENO, w, "\n", NULL);
  free(w);
  free(joined.p);
}

/* The cowfile names directly in dir, or NULL if it cannot be read. */
static List *cows_in_dir(const char *dir) {
  DIR *dp = opendir(dir);
  if (!dp) return NULL;
  List *names = xrealloc(NULL, sizeof *names);
  *names = (List){0};
  struct dirent *de;
  while ((de = readdir(dp)) != NULL) {
    size_t len = strlen(de->d_name);
    if (len > 4 && strcmp(de->d_name + len - 4, ".cow") == 0)
      list_push(names, xstrndup(de->d_name, len - 4));
  }
  closedir(dp);
  return names;
}

/* Each cowpath directory that holds a cowfile, with the names of its cowfiles. */
static void cowpath_listing(List *dirs, List **names) {
  List all = cowpath_dirs();
  for (size_t i = 0; i < all.n; i++) {
    List *found;
    if (all.v[i] == builtin_dir) {
      found = xrealloc(NULL, sizeof *found);
      *found = (List){0};
      for (size_t k = 0; k < sizeof embedded_cows / sizeof embedded_cows[0]; k++) {
        size_t len = strlen(embedded_cows[k].name);
        list_push(found, xstrndup(embedded_cows[k].name, len - 4));
      }
    } else {
      found = cows_in_dir(all.v[i]);
    }
    // A directory that cannot be read, or holds no cowfile, is left out.
    if (found && found->n) {
      names[dirs->n] = found;
      list_push(dirs, all.v[i]);
    }
  }
}

/* The directories of cowpath_listing, and room for their name lists. */
static List **listing_room(void) {
  const char *cowpath = getenv("COWPATH");
  size_t max_dirs = 2;
  for (const char *p = cowpath ? cowpath : ""; *p; p++) max_dirs += *p == ':';
  return xrealloc(NULL, max_dirs * sizeof(List *));
}

/* The cowfile names of the whole cowpath, sorted, each once. */
static List cow_names(void) {
  List dirs = {0}, all = {0}, unique = {0};
  List **names = listing_room();
  cowpath_listing(&dirs, names);
  for (size_t i = 0; i < dirs.n; i++)
    for (size_t k = 0; k < names[i]->n; k++) list_push(&all, names[i]->v[k]);
  sort_names(all.v, all.n);
  for (size_t k = 0; k < all.n; k++)
    if (!k || strcmp(all.v[k], all.v[k - 1]) != 0) list_push(&unique, all.v[k]);
  return unique;
}

/* A terminal gets the cowfiles under each directory; anything else gets their names alone. */
static void list_cowfiles(void) {
  if (isatty(STDOUT_FILENO)) {
    List dirs = {0};
    List **names = listing_room();
    cowpath_listing(&dirs, names);
    for (size_t i = 0; i < dirs.n; i++) {
      say(STDOUT_FILENO, i ? "\n" : "", "Cow files in ", dirs.v[i], ":\n", NULL);
      print_name_list(names[i]);
    }
  } else {
    List all = cow_names();
    Buf lines = {0};
    for (size_t k = 0; k < all.n; k++) {
      buf_append(&lines, all.v[k], strlen(all.v[k]));
      buf_push(&lines, '\n');
    }
    say(STDOUT_FILENO, all.n ? lines.p : "\n", NULL);
  }
  exit(0);
}

/* -r: one of the names -l lists, at random, looked up in the cowpath alone. */
static char *random_cow(void) {
  List names = cow_names();
  // With no cowfile at all, the reference looks up an undefined name, which reads as ''.
  const char *name = names.n ? names.v[arc4random_uniform((uint32_t)names.n)] : "";
  return cow_from_cowpath(name);
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
  size_t max = 0;
  for (size_t i = 0; i < n; i++) {
    size_t l = wu_display_width(lines[i], strlen(lines[i]));
    if (l > max) max = l;
  }
  size_t padw = max, max2 = max + 2;
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
  buf_push(&b, '\n');
  emit_balloon_line(&b, b0, n ? lines[0] : "", padw, b1);
  if (n >= 2) {
    for (size_t i = 1; i + 1 < n; i++) emit_balloon_line(&b, b4, lines[i], padw, b5);
    emit_balloon_line(&b, b2, lines[n - 1], padw, b3);
  }
  buf_push(&b, ' ');
  buf_repeat(&b, '-', max2);
  buf_push(&b, '\n');
  return b.p;
}

/* ---------- input ---------- */

static void read_stdin_lines(List *out) {
  Buf all = {0};
  char chunk[4096];
  ssize_t got;
  while ((got = read(STDIN_FILENO, chunk, sizeof chunk)) > 0) buf_append(&all, chunk, (size_t)got);
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
  int negative = *s == '-';
  if (*s == '-' || *s == '+') s++;
  // The value saturates as strtol's does; strtol itself would add about 2 kB to the binary.
  unsigned long limit = negative ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX;
  unsigned long value = 0;
  for (; *s >= '0' && *s <= '9'; s++) {
    unsigned long digit = (unsigned long)(*s - '0');
    value = value > (limit - digit) / 10 ? limit : value * 10 + digit;
  }
  return negative ? (long)(0 - value) : (long)value;
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

  if (o.h) display_help(0);
  if (o.l) list_cowfiles();

  eyes = cluster_prefix(o.e, 2);
  tongue = cluster_prefix(o.T, 2);
  long columns = numify(o.W);

  // Intended fix: the reference tests `unless ($ARGV[0])`, where Perl takes "" and "0" as false;
  // it then waits on stdin, while here any remaining argument selects the argument message.
  List raw = {0};
  int use_args = rest < argc;
  if (use_args) {
    if (o.n) display_help(1); // -n reads stdin only
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

  char *cow = o.r ? random_cow() : get_cow(o.f);

  say(STDOUT_FILENO, balloon, cow, NULL);
  return 0;
}
