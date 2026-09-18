/* Checks width.c against the Unicode Character Database's own grapheme break test,
 * and against the width and rendition rules cowsay relies on.
 * Run by `make check`; the data file is vendored under test/unicode/. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../width.h"

static int failures = 0;
static int checks = 0;

static void fail(const char *what, const char *detail) {
  printf("FAIL: %s: %s\n", what, detail);
  failures++;
}

/* Encode a codepoint as UTF-8, returning the bytes written. */
static size_t encode(unsigned cp, char *out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* One line of GraphemeBreakTest.txt: ÷ marks a boundary, × marks a join. */
static void run_break_case(const char *line, int lineno) {
  char text[512];
  size_t len = 0;
  size_t expected[128];
  size_t expected_count = 0;
  const char *p = line;
  while (*p && *p != '#') {
    if (strncmp(p, "\xc3\xb7", 2) == 0) {            // U+00F7 division sign: a boundary
      expected[expected_count++] = len;
      p += 2;
    } else if (strncmp(p, "\xc3\x97", 2) == 0) {     // U+00D7 multiplication sign: no boundary
      p += 2;
    } else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')) {
      char *end;
      unsigned cp = (unsigned)strtoul(p, &end, 16);
      len += encode(cp, text + len);
      p = end;
    } else {
      p++;
    }
  }
  /* Walk the clusters and compare the boundaries with the ones the file marks. */
  size_t got[128];
  size_t got_count = 0;
  for (size_t i = 0; i <= len;) {
    got[got_count++] = i;
    if (i == len) break;
    i = wu_cluster_end(text, len, i);
  }
  checks++;
  int same = got_count == expected_count;
  for (size_t k = 0; same && k < got_count; k++) same = got[k] == expected[k];
  if (!same) {
    char detail[256];
    snprintf(detail, sizeof detail, "line %d: %.80s", lineno, line);
    fail("GraphemeBreakTest", detail);
  }
}

static void check_width(const char *label, const char *text, size_t expect) {
  checks++;
  size_t got = wu_display_width(text, strlen(text));
  if (got != expect) {
    char detail[160];
    snprintf(detail, sizeof detail, "%s: want %zu columns, got %zu", label, expect, got);
    fail("width", detail);
  }
}

static void check_sgr(const char *label, const char *text, const char *expect) {
  checks++;
  WuSgr st;
  wu_sgr_clear(&st);
  wu_sgr_scan(&st, text, strlen(text));
  char out[96];
  wu_sgr_render(&st, out, sizeof out);
  if (strcmp(out, expect) != 0) {
    char detail[256];
    snprintf(detail, sizeof detail, "%s: want \"%s\", got \"%s\"", label, expect, out);
    fail("sgr", detail);
  }
}

int main(void) {
  FILE *f = fopen("test/unicode/GraphemeBreakTest.txt", "r");
  if (!f) {
    printf("FAIL: cannot open test/unicode/GraphemeBreakTest.txt\n");
    return 1;
  }
  char line[1024];
  int lineno = 0;
  while (fgets(line, sizeof line, f)) {
    lineno++;
    if (line[0] == '#' || line[0] == '\n') continue;
    run_break_case(line, lineno);
  }
  fclose(f);

  check_width("ascii", "hello", 5);
  check_width("cjk", "こんにちは", 10);
  check_width("combining", "e\xcc\x81", 1);                    // e + combining acute
  check_width("precomposed", "\xc3\xa9", 1);
  check_width("emoji", "\xf0\x9f\x90\x84", 2);                 // U+1F404 cow
  check_width("emoji zwj family",
              "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", 2);
  check_width("flag", "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5", 2);  // JP
  check_width("keycap", "1\xef\xb8\x8f\xe2\x83\xa3", 2);       // 1 + VS16 + combining keycap
  check_width("text presentation", "\xe2\x98\x80\xef\xb8\x8e", 1);
  check_width("devanagari conjunct", "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7", 1);
  check_width("escape only", "\033[31m", 0);
  check_width("escaped text", "\033[1;31mred\033[0m", 3);
  check_width("osc", "\033]0;title\007x", 1);

  wu_set_ambiguous_wide(0);
  check_width("ambiguous narrow", "\xc2\xa7", 1);              // section sign
  wu_set_ambiguous_wide(1);
  check_width("ambiguous wide", "\xc2\xa7", 2);
  wu_set_ambiguous_wide(0);

  check_sgr("plain", "no escapes", "");
  check_sgr("fg", "\033[31mred", "\033[31m");
  check_sgr("fg then reset", "\033[31mred\033[0m", "");
  check_sgr("fg and bg", "\033[31m\033[44mx", "\033[31;44m");
  check_sgr("truecolor", "\033[38;2;215;119;87mclawd", "\033[38;2;215;119;87m");
  check_sgr("256 colour bg", "\033[48;5;236mx", "\033[48;5;236m");
  check_sgr("bold kept", "\033[1;31mx", "\033[1;31m");
  check_sgr("bold cleared", "\033[1;31mx\033[22m", "\033[31m");
  check_sgr("default fg", "\033[31mx\033[39m", "");

  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
