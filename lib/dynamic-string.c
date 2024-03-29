/*
 * Copyright (c) 2008, 2009, 2010, 2011 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "dynamic-string.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "timeval.h"
#include "util.h"

void
ds_init(struct ds *ds)
{
    ds->string = NULL;
    ds->length = 0;
    ds->allocated = 0;
}

void
ds_clear(struct ds *ds)
{
    ds->length = 0;
}

void
ds_truncate(struct ds *ds, size_t new_length)
{
    if (ds->length > new_length) {
        ds->length = new_length;
        ds->string[new_length] = '\0';
    }
}

void
ds_reserve(struct ds *ds, size_t min_length)
{
    if (min_length > ds->allocated || !ds->string) {
        ds->allocated += MAX(min_length, ds->allocated);
        ds->allocated = MAX(8, ds->allocated);
        ds->string = xrealloc(ds->string, ds->allocated + 1);
    }
}

char *
ds_put_uninit(struct ds *ds, size_t n)
{
    ds_reserve(ds, ds->length + n);
    ds->length += n;
    ds->string[ds->length] = '\0';
    return &ds->string[ds->length - n];
}

void
ds_put_char__(struct ds *ds, char c)
{
    *ds_put_uninit(ds, 1) = c;
}

/* Appends unicode code point 'uc' to 'ds' in UTF-8 encoding. */
void
ds_put_utf8(struct ds *ds, int uc)
{
    if (uc <= 0x7f) {
        ds_put_char(ds, uc);
    } else if (uc <= 0x7ff) {
        ds_put_char(ds, 0xc0 | (uc >> 6));
        ds_put_char(ds, 0x80 | (uc & 0x3f));
    } else if (uc <= 0xffff) {
        ds_put_char(ds, 0xe0 | (uc >> 12));
        ds_put_char(ds, 0x80 | ((uc >> 6) & 0x3f));
        ds_put_char(ds, 0x80 | (uc & 0x3f));
    } else if (uc <= 0x10ffff) {
        ds_put_char(ds, 0xf0 | (uc >> 18));
        ds_put_char(ds, 0x80 | ((uc >> 12) & 0x3f));
        ds_put_char(ds, 0x80 | ((uc >> 6) & 0x3f));
        ds_put_char(ds, 0x80 | (uc & 0x3f));
    } else {
        /* Invalid code point.  Insert the Unicode general substitute
         * REPLACEMENT CHARACTER. */
        ds_put_utf8(ds, 0xfffd);
    }
}

void
ds_put_char_multiple(struct ds *ds, char c, size_t n)
{
    memset(ds_put_uninit(ds, n), c, n);
}

void
ds_put_buffer(struct ds *ds, const char *s, size_t n)
{
    memcpy(ds_put_uninit(ds, n), s, n);
}

void
ds_put_cstr(struct ds *ds, const char *s)
{
    size_t s_len = strlen(s);
    memcpy(ds_put_uninit(ds, s_len), s, s_len);
}

void
ds_put_and_free_cstr(struct ds *ds, char *s)
{
    ds_put_cstr(ds, s);
    free(s);
}

void
ds_put_format(struct ds *ds, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    ds_put_format_valist(ds, format, args);
    va_end(args);
}

void
ds_put_format_valist(struct ds *ds, const char *format, va_list args_)
{
    va_list args;
    size_t available;
    int needed;

    va_copy(args, args_);
    available = ds->string ? ds->allocated - ds->length + 1 : 0;
    needed = vsnprintf(&ds->string[ds->length], available, format, args);
    va_end(args);

    if (needed < available) {
        ds->length += needed;
    } else {
        ds_reserve(ds, ds->length + needed);

        va_copy(args, args_);
        available = ds->allocated - ds->length + 1;
        needed = vsnprintf(&ds->string[ds->length], available, format, args);
        va_end(args);

        assert(needed < available);
        ds->length += needed;
    }
}

void
ds_put_printable(struct ds *ds, const char *s, size_t n)
{
    ds_reserve(ds, ds->length + n);
    while (n-- > 0) {
        unsigned char c = *s++;
        if (c < 0x20 || c > 0x7e || c == '\\' || c == '"') {
            ds_put_format(ds, "\\%03o", (int) c);
        } else {
            ds_put_char(ds, c);
        }
    }
}

/* Writes the current time to 'string' based on 'template'.
 * The current time is either localtime or UTC based on 'utc'. */
void
ds_put_strftime(struct ds *ds, const char *template, bool utc)
{
    const struct tm *tm;
    time_t now = time_wall();
    if (utc) {
        tm = gmtime(&now);
    } else {
        tm = localtime(&now);
    }

    for (;;) {
        size_t avail = ds->string ? ds->allocated - ds->length + 1 : 0;
        size_t used = strftime(&ds->string[ds->length], avail, template, tm);
        if (used) {
            ds->length += used;
            return;
        }
        ds_reserve(ds, ds->length + (avail < 32 ? 64 : 2 * avail));
    }
}

int
ds_get_line(struct ds *ds, FILE *file)
{
    ds_clear(ds);
    for (;;) {
        int c = getc(file);
        if (c == EOF) {
            return ds->length ? 0 : EOF;
        } else if (c == '\n') {
            return 0;
        } else {
            ds_put_char(ds, c);
        }
    }
}

/* Reads a line from 'file' into 'ds', clearing anything initially in 'ds'.
 * Deletes comments introduced by "#" and skips lines that contains only white
 * space (after deleting comments).
 *
 * Returns 0 if successful, EOF if no non-blank line was found. */
int
ds_get_preprocessed_line(struct ds *ds, FILE *file)
{
    while (!ds_get_line(ds, file)) {
        char *line = ds_cstr(ds);
        char *comment;

        /* Delete comments. */
        comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
        }

        /* Return successfully unless the line is all spaces. */
        if (line[strspn(line, " \t\n")] != '\0') {
            return 0;
        }
    }
    return EOF;
}

char *
ds_cstr(struct ds *ds)
{
    if (!ds->string) {
        ds_reserve(ds, 0);
    }
    ds->string[ds->length] = '\0';
    return ds->string;
}

const char *
ds_cstr_ro(const struct ds *ds)
{
    return ds_cstr((struct ds *) ds);
}

/* Returns a null-terminated string representing the current contents of 'ds',
 * which the caller is expected to free with free(), then clears the contents
 * of 'ds'. */
char *
ds_steal_cstr(struct ds *ds)
{
    char *s = ds_cstr(ds);
    ds_init(ds);
    return s;
}

void
ds_destroy(struct ds *ds)
{
    free(ds->string);
}

/* Swaps the content of 'a' and 'b'. */
void
ds_swap(struct ds *a, struct ds *b)
{
    struct ds temp = *a;
    *a = *b;
    *b = temp;
}

/* Writes the 'size' bytes in 'buf' to 'string' as hex bytes arranged 16 per
 * line.  Numeric offsets are also included, starting at 'ofs' for the first
 * byte in 'buf'.  If 'ascii' is true then the corresponding ASCII characters
 * are also rendered alongside. */
void
ds_put_hex_dump(struct ds *ds, const void *buf_, size_t size,
                uintptr_t ofs, bool ascii)
{
  const uint8_t *buf = buf_;
  const size_t per_line = 16; /* Maximum bytes per line. */

  while (size > 0)
    {
      size_t start, end, n;
      size_t i;

      /* Number of bytes on this line. */
      start = ofs % per_line;
      end = per_line;
      if (end - start > size)
        end = start + size;
      n = end - start;

      /* Print line. */
      ds_put_format(ds, "%08jx  ", (uintmax_t) ROUND_DOWN(ofs, per_line));
      for (i = 0; i < start; i++)
        ds_put_format(ds, "   ");
      for (; i < end; i++)
        ds_put_format(ds, "%02hhx%c",
                buf[i - start], i == per_line / 2 - 1? '-' : ' ');
      if (ascii)
        {
          for (; i < per_line; i++)
            ds_put_format(ds, "   ");
          ds_put_format(ds, "|");
          for (i = 0; i < start; i++)
            ds_put_format(ds, " ");
          for (; i < end; i++) {
              int c = buf[i - start];
              ds_put_char(ds, c >= 32 && c < 127 ? c : '.');
          }
          for (; i < per_line; i++)
            ds_put_format(ds, " ");
          ds_put_format(ds, "|");
        }
      ds_put_format(ds, "\n");

      ofs += n;
      buf += n;
      size -= n;
    }
}

int
ds_last(const struct ds *ds)
{
    return ds->length > 0 ? (unsigned char) ds->string[ds->length - 1] : EOF;
}

void
ds_chomp(struct ds *ds, int c)
{
    if (ds->length > 0 && ds->string[ds->length - 1] == (char) c) {
        ds->string[--ds->length] = '\0';
    }
}
