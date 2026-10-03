/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * contract.c — The frozen command line of /orchestrate (SPEC.md
 * sections 5 and 6).
 *
 * Hand written, allocation free, single pass JSON scanner of the
 * command shapes: a task descriptor (bare or under "run"), or an
 * explicit command ("status", "result").  Only what the contract
 * freezes is accepted; everything else is a readable "invalid"
 * answer with its reason.
 */

#include "contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *
orc_invalid_reason (enum orc_invalid reason)
{
  switch (reason)
    {
    case ORC_INVALID_LINE:   return "line";
    case ORC_INVALID_JSON:   return "json";
    case ORC_INVALID_FIELD:  return "field";
    case ORC_INVALID_TYPE:   return "type";
    case ORC_INVALID_VALUE:  return "value";
    case ORC_INVALID_KEY:    return "key";
    case ORC_INVALID_TOOLONG: return "toolong";
    }
  return "line";
}

/* ---------------------------------------------------------------------
 *  Scanner (the same proven shape as the /db contract layer).
 * ------------------------------------------------------------------- */

struct scanner
  {
    const char *s;
    size_t pos;
    size_t len;
    char *scratch;
    size_t scratch_cap;
    size_t scratch_len;
    int err;                       /* 0 ok, 1 JSON error, 2 arena full */
  };

static void
fail (struct scanner *sc, int err)
{
  if (sc->err == 0)
    sc->err = err;
}

static char *
arena_take (struct scanner *sc, size_t n)
{
  char *p;

  if (sc->scratch_len + n > sc->scratch_cap)
    {
      fail (sc, 2);
      return NULL;
    }
  p = sc->scratch + sc->scratch_len;
  sc->scratch_len += n;
  return p;
}

static void
skip_ws (struct scanner *sc)
{
  while (sc->pos < sc->len)
    {
      char c = sc->s[sc->pos];

      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        sc->pos++;
      else
        break;
    }
}

static int
peek (struct scanner *sc)
{
  return sc->pos < sc->len ? (unsigned char) sc->s[sc->pos] : -1;
}

static int
next (struct scanner *sc)
{
  return sc->pos < sc->len ? (unsigned char) sc->s[sc->pos++] : -1;
}

static size_t
utf8_len (unsigned long cp)
{
  if (cp < 0x80)
    return 1;
  if (cp < 0x800)
    return 2;
  if (cp < 0x10000)
    return 3;
  return 4;
}

static void
put_utf8 (char **out, unsigned long cp)
{
  char *p = *out;

  if (cp < 0x80)
    p[0] = (char) cp;
  else if (cp < 0x800)
    {
      p[0] = (char) (0xC0 | (cp >> 6));
      p[1] = (char) (0x80 | (cp & 0x3F));
    }
  else if (cp < 0x10000)
    {
      p[0] = (char) (0xE0 | (cp >> 12));
      p[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
      p[2] = (char) (0x80 | (cp & 0x3F));
    }
  else
    {
      p[0] = (char) (0xF0 | (cp >> 18));
      p[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
      p[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
      p[3] = (char) (0x80 | (cp & 0x3F));
    }
  *out = p + utf8_len (cp);
}

static unsigned long
read_hex4 (struct scanner *sc)
{
  unsigned long cp = 0;

  for (int i = 0; i < 4; i++)
    {
      int c = next (sc);
      int v;

      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else
        {
          fail (sc, 1);
          return (unsigned long) -1;
        }
      cp = cp * 16 + (unsigned long) v;
    }
  return cp;
}

/* Decode a JSON string into the arena (reservation trimmed at the
 * closing quote, so at most one reservation is live at a time). */
static const char *
parse_string (struct scanner *sc, size_t *len_out)
{
  size_t base = sc->scratch_len;
  char *start;
  char *out;

  if (next (sc) != '"')
    {
      fail (sc, 1);
      return NULL;
    }
  start = arena_take (sc, sc->len - sc->pos + 1);
  if (start == NULL)
    return NULL;
  out = start;

  for (;;)
    {
      int c = peek (sc);

      if (c < 0 || c < 0x20)
        {
          fail (sc, 1);
          return NULL;
        }
      if (c == '"')
        {
          sc->pos++;
          *out = '\0';
          sc->scratch_len = base + (size_t) (out - start) + 1;
          if (len_out)
            *len_out = (size_t) (out - start);
          return start;
        }
      if (c == '\\')
        {
          sc->pos++;
          int e = next (sc);

          switch (e)
            {
            case '"':  *out++ = '"';  break;
            case '\\': *out++ = '\\'; break;
            case '/':  *out++ = '/';  break;
            case 'b':  *out++ = '\b'; break;
            case 'f':  *out++ = '\f'; break;
            case 'n':  *out++ = '\n'; break;
            case 'r':  *out++ = '\r'; break;
            case 't':  *out++ = '\t'; break;
            case 'u':
              {
                unsigned long cp = read_hex4 (sc);

                if (sc->err)
                  return NULL;
                if (cp >= 0xD800 && cp <= 0xDBFF
                    && sc->pos + 1 < sc->len
                    && sc->s[sc->pos] == '\\'
                    && sc->s[sc->pos + 1] == 'u')
                  {
                    unsigned long lo;

                    sc->pos += 2;
                    lo = read_hex4 (sc);
                    if (sc->err)
                      return NULL;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                      cp = 0x10000 + ((cp - 0xD800) << 10)
                           + (lo - 0xDC00);
                    else
                      {
                        fail (sc, 1);
                        return NULL;
                      }
                  }
                put_utf8 (&out, cp);
                break;
              }
            default:
              fail (sc, 1);
              return NULL;
            }
          continue;
        }
      *out++ = (char) c;
      sc->pos++;
    }
}

/* Walk one JSON value without decoding; returns its [start, end)
 * span inside the LINE. */
static int
skip_value (struct scanner *sc, size_t *start_out, size_t *end_out)
{
  size_t start = sc->pos;
  int c = peek (sc);

  if (c == '"')
    {
      size_t ignored;

      if (parse_string (sc, &ignored) == NULL)
        return -1;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == '{' || c == '[')
    {
      int depth = 0;

      while (sc->pos < sc->len)
        {
          int d = peek (sc);

          if (d == '"')
            {
              size_t ignored;

              if (parse_string (sc, &ignored) == NULL)
                return -1;
              continue;
            }
          sc->pos++;
          if (d == '{' || d == '[')
            depth++;
          else if (d == '}' || d == ']')
            {
              depth--;
              if (depth == 0)
                {
                  *start_out = start;
                  *end_out = sc->pos;
                  return 0;
                }
              if (depth < 0)
                {
                  fail (sc, 1);
                  return -1;
                }
            }
        }
      fail (sc, 1);
      return -1;
    }
  if (c == 't' && sc->len - sc->pos >= 4
      && memcmp (sc->s + sc->pos, "true", 4) == 0)
    {
      sc->pos += 4;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == 'f' && sc->len - sc->pos >= 5
      && memcmp (sc->s + sc->pos, "false", 5) == 0)
    {
      sc->pos += 5;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == 'n' && sc->len - sc->pos >= 4
      && memcmp (sc->s + sc->pos, "null", 4) == 0)
    {
      sc->pos += 4;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == '-' || (c >= '0' && c <= '9'))
    {
      sc->pos++;
      for (;;)
        {
          int d = peek (sc);

          if ((d >= '0' && d <= '9') || d == '.' || d == 'e'
              || d == 'E' || d == '+' || d == '-')
            sc->pos++;
          else
            break;
        }
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  fail (sc, 1);
  return -1;
}

/* Copy a span of the LINE into the arena, verbatim, NUL
 * terminated. */
static const char *
copy_raw (struct scanner *sc, size_t start, size_t end)
{
  char *p = arena_take (sc, end - start + 1);

  if (p == NULL)
    return NULL;
  memcpy (p, sc->s + start, end - start);
  p[end - start] = '\0';
  return p;
}

/* ---------------------------------------------------------------------
 *  Descriptor binding (SPEC.md section 6).
 *  The scanner stands ON the opening '{' of the descriptor.
 * ------------------------------------------------------------------- */

static void
set_detail (struct orc_request *req, const char *key)
{
  size_t n = strlen (key);

  if (n > sizeof req->invalid_detail - 1)
    n = sizeof req->invalid_detail - 1;
  memcpy (req->invalid_detail, key, n);
  req->invalid_detail[n] = '\0';
}

static int
parse_descriptor (struct scanner *sc, struct orc_request *req)
{
  struct orc_descriptor *d = &req->desc;
  size_t desc_start;
  size_t desc_end;
  bool have_instances = false;
  bool have_topologies = false;
  bool have_input = false;
  bool have_aggregate = false;

  desc_start = sc->pos;
  if (next (sc) != '{')
    {
      req->invalid = ORC_INVALID_JSON;
      return -1;
    }
  skip_ws (sc);
  if (peek (sc) == '}')
    {
      sc->pos++;
      desc_end = sc->pos;
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "instances");
      return -1;
    }

  for (;;)
    {
      const char *key;
      size_t keylen = 0;
      char keybuf[32];

      skip_ws (sc);
      key = parse_string (sc, &keylen);
      if (key == NULL || keylen >= sizeof keybuf)
        {
          if (sc->err == 0)
            {
              req->invalid = ORC_INVALID_JSON;
              return -1;
            }
          req->invalid = ORC_INVALID_JSON;
          return -1;
        }
      memcpy (keybuf, key, keylen);
      keybuf[keylen] = '\0';

      skip_ws (sc);
      if (next (sc) != ':')
        {
          req->invalid = ORC_INVALID_JSON;
          return -1;
        }
      skip_ws (sc);

      if (strcmp (keybuf, "instances") == 0)
        {
          size_t s0, e0;

          if (skip_value (sc, &s0, &e0) < 0)
            {
              req->invalid = ORC_INVALID_JSON;
              return -1;
            }
          if (e0 - s0 < 1)
            {
              req->invalid = ORC_INVALID_TYPE;
              set_detail (req, "instances");
              return -1;
            }
          /* A pure integer is required: no fraction, no exponent. */
          {
            const char *p = sc->s + s0;
            size_t n = e0 - s0;

            if (*p == '-')
              {
                p++;
                n--;
              }
            if (n == 0)
              {
                req->invalid = ORC_INVALID_TYPE;
                set_detail (req, "instances");
                return -1;
              }
            for (size_t i = 0; i < n; i++)
              if (p[i] < '0' || p[i] > '9')
                {
                  req->invalid = ORC_INVALID_TYPE;
                  set_detail (req, "instances");
                  return -1;
                }
            d->instances = atoi (sc->s + s0);
          }
          if (d->instances < 1 || d->instances > ORC_MAX_INSTANCES)
            {
              req->invalid = ORC_INVALID_VALUE;
              set_detail (req, "instances");
              return -1;
            }
          have_instances = true;
        }
      else if (strcmp (keybuf, "topologies") == 0)
        {
          if (peek (sc) != '[')
            {
              req->invalid = ORC_INVALID_TYPE;
              set_detail (req, "topologies");
              return -1;
            }
          sc->pos++;
          skip_ws (sc);
          if (peek (sc) == ']')
            {
              sc->pos++;
            }
          else
            for (;;)
              {
                const char *t;
                size_t tlen = 0;

                skip_ws (sc);
                t = parse_string (sc, &tlen);
                if (t == NULL)
                  {
                    req->invalid = ORC_INVALID_JSON;
                    return -1;
                  }
                if (tlen == 0 || tlen > ORC_MAX_TOPOLOGY
                    || d->n_topologies >= ORC_MAX_TOPOLOGIES)
                  {
                    req->invalid = ORC_INVALID_VALUE;
                    set_detail (req, "topologies");
                    return -1;
                  }
                strcpy (d->topologies[d->n_topologies++], t);
                skip_ws (sc);
                {
                  int c2 = next (sc);

                  if (c2 == ',')
                    continue;
                  if (c2 == ']')
                    break;
                  req->invalid = ORC_INVALID_JSON;
                  return -1;
                }
            }
          have_topologies = true;
        }
      else if (strcmp (keybuf, "input") == 0)
        {
          if (peek (sc) != '[')
            {
              req->invalid = ORC_INVALID_TYPE;
              set_detail (req, "input");
              return -1;
            }
          sc->pos++;
          skip_ws (sc);
          if (peek (sc) == ']')
            {
              sc->pos++;
            }
          else
            for (;;)
              {
                size_t s0, e0;
                const char *num;

                skip_ws (sc);
                if (skip_value (sc, &s0, &e0) < 0)
                  {
                    req->invalid = ORC_INVALID_JSON;
                    return -1;
                  }
                if (d->n_input >= ORC_MAX_INPUT)
                  {
                    req->invalid = ORC_INVALID_VALUE;
                    set_detail (req, "input");
                    return -1;
                  }
                num = copy_raw (sc, s0, e0);
                if (num == NULL)
                  {
                    req->invalid = ORC_INVALID_JSON;
                    return -1;
                  }
                d->input[d->n_input++] = strtod (num, NULL);
                skip_ws (sc);
                {
                  int c2 = next (sc);

                  if (c2 == ',')
                    continue;
                  if (c2 == ']')
                    break;
                  req->invalid = ORC_INVALID_JSON;
                  return -1;
                }
            }
          have_input = true;
        }
      else if (strcmp (keybuf, "aggregate") == 0)
        {
          const char *a;
          size_t alen = 0;

          a = parse_string (sc, &alen);
          if (a == NULL)
            {
              req->invalid = ORC_INVALID_JSON;
              return -1;
            }
          if (strcmp (a, "majority") != 0 && strcmp (a, "mean") != 0)
            {
              req->invalid = ORC_INVALID_VALUE;
              set_detail (req, "aggregate");
              return -1;
            }
          strcpy (d->aggregate, a);
          have_aggregate = true;
        }
      else
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, keybuf);
          return -1;
        }

      skip_ws (sc);
      {
        int c = next (sc);

        if (c == ',')
          continue;
        if (c == '}')
          {
            desc_end = sc->pos;
            break;
          }
        req->invalid = ORC_INVALID_JSON;
        return -1;
      }
    }

  if (!have_instances)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "instances");
      return -1;
    }
  if (!have_topologies || d->n_topologies == 0)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "topologies");
      return -1;
    }
  if (!have_input || d->n_input == 0)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "input");
      return -1;
    }
  if (!have_aggregate)
    {
      req->invalid = ORC_INVALID_FIELD;
      set_detail (req, "aggregate");
      return -1;
    }

  /* Persist the descriptor verbatim: what the caller wrote is
   * what the base keeps (SPEC.md section 6). */
  {
    size_t n = desc_end - desc_start;

    if (n >= sizeof d->raw)
      n = sizeof d->raw - 1;
    memcpy (d->raw, sc->s + desc_start, n);
    d->raw[n] = '\0';
  }
  return 0;
}

/* ---------------------------------------------------------------------
 *  orc_parse — one command line to one request.
 * ------------------------------------------------------------------- */

void
orc_parse (const char *line, size_t len,
           char *scratch, size_t scratch_cap,
           struct orc_request *req)
{
  struct scanner sc;
  bool have_command = false;
  bool have_run = false;
  bool have_descriptor_keys = false;

  memset (req, 0, sizeof *req);
  req->command = ORC_CMD_INVALID;
  req->invalid = ORC_INVALID_LINE;

  sc.s = line;
  sc.pos = 0;
  sc.len = len;
  sc.scratch = scratch;
  sc.scratch_cap = scratch_cap;
  sc.scratch_len = 0;
  sc.err = 0;

  skip_ws (&sc);
  if (peek (&sc) != '{')
    return;
  sc.pos++;

  skip_ws (&sc);
  if (peek (&sc) == '}')
    {
      sc.pos++;
      skip_ws (&sc);
      if (sc.pos == sc.len)
        req->invalid = ORC_INVALID_FIELD;   /* empty is not a command */
      else
        req->invalid = ORC_INVALID_LINE;
      set_detail (req, "run");
      return;
    }

  for (;;)
    {
      const char *key;
      size_t keylen = 0;
      char keybuf[32];

      skip_ws (&sc);
      key = parse_string (&sc, &keylen);
      if (key == NULL)
        {
          req->invalid = ORC_INVALID_JSON;
          return;
        }
      if (keylen >= sizeof keybuf)
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, key);
          return;
        }
      memcpy (keybuf, key, keylen);
      keybuf[keylen] = '\0';

      skip_ws (&sc);
      if (next (&sc) != ':')
        {
          req->invalid = ORC_INVALID_JSON;
          return;
        }
      skip_ws (&sc);

      if (strcmp (keybuf, "run") == 0)
        {
          if (parse_descriptor (&sc, req) < 0)
            {
              if (req->invalid == ORC_INVALID_LINE)
                req->invalid = ORC_INVALID_JSON;
              return;
            }
          have_run = true;
        }
      else if (strcmp (keybuf, "command") == 0)
        {
          const char *c;
          size_t clen = 0;

          c = parse_string (&sc, &clen);
          if (c == NULL)
            {
              req->invalid = ORC_INVALID_JSON;
              return;
            }
          if (strcmp (c, "status") == 0)
            req->command = ORC_CMD_STATUS;
          else if (strcmp (c, "result") == 0)
            req->command = ORC_CMD_RESULT;
          else
            {
              req->invalid = ORC_INVALID_VALUE;
              set_detail (req, "command");
              return;
            }
          have_command = true;
        }
      else if (strcmp (keybuf, "instances") == 0
               || strcmp (keybuf, "topologies") == 0
               || strcmp (keybuf, "input") == 0
               || strcmp (keybuf, "aggregate") == 0)
        {
          /* A bare descriptor: rewind onto the key and parse the
           * whole object as the descriptor.  Mixed with an
           * explicit "run" it is not a command shape. */
          if (have_run)
            {
              req->invalid = ORC_INVALID_KEY;
              set_detail (req, keybuf);
              return;
            }
          while (sc.pos > 0 && sc.s[sc.pos] != '{')
            sc.pos--;
          if (sc.s[sc.pos] != '{')
            {
              req->invalid = ORC_INVALID_JSON;
              return;
            }
          if (parse_descriptor (&sc, req) < 0)
            {
              if (req->invalid == ORC_INVALID_LINE)
                req->invalid = ORC_INVALID_JSON;
              return;
            }
          have_descriptor_keys = true;
          /* A bare descriptor IS the whole top-level object: it
           * was consumed entirely, the command is complete. */
          break;
        }
      else
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, keybuf);
          return;
        }

      skip_ws (&sc);
      {
        int c = next (&sc);

        if (c == ',')
          continue;
        if (c == '}')
          break;
        req->invalid = ORC_INVALID_JSON;
        return;
      }
    }

  skip_ws (&sc);
  if (sc.pos != sc.len)
    {
      req->invalid = ORC_INVALID_LINE;
      return;
    }

  if (have_run || have_descriptor_keys)
    {
      if (have_command)
        {
          req->invalid = ORC_INVALID_KEY;
          set_detail (req, "command");
          return;
        }
      req->command = ORC_CMD_RUN;
      return;
    }
  if (have_command)
    return;                                 /* STATUS or RESULT */

  req->invalid = ORC_INVALID_FIELD;
  set_detail (req, "run");
}

/* ---------------------------------------------------------------------
 *  Response builders
 * ------------------------------------------------------------------- */

size_t
orc_response_invalid (char *buf, size_t cap,
                      enum orc_invalid reason, const char *detail)
{
  const char *r = detail != NULL && detail[0] != '\0'
                    ? detail : orc_invalid_reason (reason);
  int n = snprintf (buf, cap, "{\"invalid\": \"%s\"}", r);

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}

size_t
orc_response_empty (char *buf, size_t cap)
{
  int n = snprintf (buf, cap, "{\"empty\": true}");

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}
