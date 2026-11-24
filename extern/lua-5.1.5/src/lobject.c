/*
** $Id: lobject.c,v 2.22.1.1 2007/12/27 13:02:25 roberto Exp $
** Some generic functions over Lua objects
** See Copyright Notice in lua.h
*/

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lobject_c
#define LUA_CORE

#include "lua.h"

#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "lvm.h"



const TValue luaO_nilobject_ = {{NULL}, LUA_TNIL, 0};


/*
** converts an integer to a "floating point byte", represented as
** (eeeeexxx), where the real value is (1xxx) * 2^(eeeee - 1) if
** eeeee != 0 and (xxx) otherwise.
*/
int luaO_int2fb (unsigned int x) {
  int e = 0;  /* expoent */
  while (x >= 16) {
    x = (x+1) >> 1;
    e++;
  }
  if (x < 8) return x;
  else return ((e+1) << 3) | (cast_int(x) - 8);
}


/* converts back */
int luaO_fb2int (int x) {
  int e = (x >> 3) & 31;
  if (e == 0) return x;
  else return ((x & 7)+8) << (e - 1);
}


int luaO_log2 (unsigned int x) {
  static const lu_byte log_2[256] = {
    0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8
  };
  int l = -1;
  while (x >= 256) { l += 8; x >>= 8; }
  return l + log_2[x];

}


int luaO_rawequalObj (const TValue *t1, const TValue *t2) {
  if (ttype(t1) != ttype(t2)) return 0;
  else switch (ttype(t1)) {
    case LUA_TNIL:
      return 1;
    case LUA_TNUMBER:
      return luai_numeq(nvalue(t1), nvalue(t2));
    case LUA_TBOOLEAN:
      return bvalue(t1) == bvalue(t2);  /* boolean true must be 1 !! */
    case LUA_TLIGHTUSERDATA:
      return pvalue(t1) == pvalue(t2);
    default:
      lua_assert(iscollectable(t1));
      return gcvalue(t1) == gcvalue(t2);
  }
}


/* WoWee: the client's strtod reads neither "inf" and "nan" nor a hexadecimal
** number, which the language reads itself, as 32 bits. */
int luaO_str2d (const char *s, lua_Number *result) {
  char *endptr;
  const char *p = s;
  while (isspace(cast(unsigned char, *p))) p++;
  if (*p == '-' || *p == '+') p++;
  if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
    unsigned long long h = 0;
    const char *q = p + 2;
    for (; isxdigit(cast(unsigned char, *q)); q++)
      if (h <= 0xFFFFFFFFull)
        h = h * 16 + cast(unsigned, isdigit(cast(unsigned char, *q)) ? *q - '0' : tolower(*q) - 'a' + 10);
    if (q == p + 2) return 0;
    if (h > 0xFFFFFFFFull) h = 0xFFFFFFFFull;
    else if (s != p && p[-1] == '-') h = (0x100000000ull - h) & 0xFFFFFFFFull;
    *result = cast_num(h);
    endptr = cast(char *, q);
  }
  else {
    if (!isdigit(cast(unsigned char, *p)) && *p != '.') return 0;
    *result = lua_str2number(s, &endptr);
    if (endptr == s) return 0;  /* conversion failed */
  }
  if (*endptr == '\0') return 1;  /* most common case */
  while (isspace(cast(unsigned char, *endptr))) endptr++;
  if (*endptr != '\0') return 0;  /* invalid trailing characters? */
  return 1;
}


/* WoWee: the digits of what is no number, "1#INF" and so on, put through a
** format of `digits` digits as any others: what follows the last of them counts
** as five or more. */
static void nonfinite (char *s, const char *man, char conv, int prec, int alt) {
  int digits = (conv == 'g' || conv == 'G') ? (prec > 0 ? prec : 1) : prec + 1;
  int i, k = cast_int(strlen(man)), g = (conv == 'g' || conv == 'G');
  char *d = s;
  for (i = 0; i < digits; i++) {
    if (i == 1) *d++ = '.';
    *d++ = i < k ? man[i] : '0';
  }
  if (digits < k && man[digits] >= '5') d[-1]++;
  if (digits == 1 && alt) *d++ = '.';
  *d = '\0';
  if (g && !alt && digits > 1) {
    while (d[-1] == '0') *--d = '\0';
    if (d[-1] == '.') *--d = '\0';
  }
  if (conv == 'e' || conv == 'E') sprintf(d, "%c+000", conv);
}


/* WoWee: a number as the client's runtime (MSVC 8) writes it by a format of
** e, E, f, g or G: three digits to an exponent at least, 1.#INF, -1.#IND (what
** 0/0 gives) and 1.#QNAN. Addons compare with them. */
void luaO_crtnumber (char *s, const char *form, lua_Number n) {
  char num[480], f[8], *e;
  int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = 0, prec = 6, len, pad, neg = signbit(n) != 0;
  char conv, sign;
  const char *p = form + 1;
  for (;; p++) {
    if (*p == '-') left = 1;
    else if (*p == '+') plus = 1;
    else if (*p == ' ') space = 1;
    else if (*p == '#') alt = 1;
    else if (*p == '0') zero = 1;
    else break;
  }
  while (isdigit(cast(unsigned char, *p))) width = width * 10 + (*p++ - '0');
  if (*p == '.') {
    prec = 0;
    for (p++; isdigit(cast(unsigned char, *p)); p++) prec = prec * 10 + (*p - '0');
  }
  if (prec > 99) prec = 99;
  conv = *p;
  if (n - n != 0) {
    volatile double none = 0.0;
    const double ind = none / none;
    const char *man = "1#INF";
    if (n != n) {
      if (neg == (signbit(ind) != 0)) { man = "1#IND"; neg = 1; }
      else { man = "1#QNAN"; neg = 0; }
    }
    nonfinite(num, man, conv, prec, alt);
  }
  else {
    sprintf(f, "%%%s.*%c", alt ? "#" : "", conv);
    sprintf(num, f, prec, fabs(n));
    if (conv != 'f' && (e = strpbrk(num, "eE")) != NULL) {
      int digits = cast_int(strlen(e + 2));
      if (digits < 3) {
        memmove(e + 2 + (3 - digits), e + 2, digits + 1);
        memset(e + 2, '0', 3 - digits);
      }
    }
  }
  sign = neg ? '-' : plus ? '+' : space ? ' ' : '\0';
  len = cast_int(strlen(num)) + (sign != '\0');
  pad = width > len ? width - len : 0;
  if (!left && !zero) { memset(s, ' ', pad); s += pad; }
  if (sign != '\0') *s++ = sign;
  if (!left && zero) { memset(s, '0', pad); s += pad; }
  strcpy(s, num);
  s += strlen(num);
  if (left) { memset(s, ' ', pad); s += pad; }
  *s = '\0';
}



static void pushstr (lua_State *L, const char *str) {
  setsvalue2s(L, L->top, luaS_new(L, str));
  incr_top(L);
}


/* this function handles only `%d', `%c', %f, %p, and `%s' formats */
const char *luaO_pushvfstring (lua_State *L, const char *fmt, va_list argp) {
  int n = 1;
  pushstr(L, "");
  for (;;) {
    const char *e = strchr(fmt, '%');
    if (e == NULL) break;
    setsvalue2s(L, L->top, luaS_newlstr(L, fmt, e-fmt));
    incr_top(L);
    switch (*(e+1)) {
      case 's': {
        const char *s = va_arg(argp, char *);
        if (s == NULL) s = "(null)";
        pushstr(L, s);
        break;
      }
      case 'c': {
        char buff[2];
        buff[0] = cast(char, va_arg(argp, int));
        buff[1] = '\0';
        pushstr(L, buff);
        break;
      }
      case 'd': {
        setnvalue(L->top, cast_num(va_arg(argp, int)));
        incr_top(L);
        break;
      }
      case 'f': {
        setnvalue(L->top, cast_num(va_arg(argp, l_uacNumber)));
        incr_top(L);
        break;
      }
      case 'p': {
        char buff[4*sizeof(void *) + 8]; /* should be enough space for a `%p' */
        sprintf(buff, "%p", va_arg(argp, void *));
        pushstr(L, buff);
        break;
      }
      case '%': {
        pushstr(L, "%");
        break;
      }
      default: {
        char buff[3];
        buff[0] = '%';
        buff[1] = *(e+1);
        buff[2] = '\0';
        pushstr(L, buff);
        break;
      }
    }
    n += 2;
    fmt = e+2;
  }
  pushstr(L, fmt);
  luaV_concat(L, n+1, cast_int(L->top - L->base) - 1);
  L->top -= n;
  return svalue(L->top - 1);
}


const char *luaO_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *msg;
  va_list argp;
  va_start(argp, fmt);
  msg = luaO_pushvfstring(L, fmt, argp);
  va_end(argp);
  return msg;
}


void luaO_chunkid (char *out, const char *source, size_t bufflen) {
  if (*source == '=') {
    strncpy(out, source+1, bufflen);  /* remove first char */
    out[bufflen-1] = '\0';  /* ensures null termination */
  }
  else {  /* out = "source", or "...source" */
    if (*source == '@') {
      size_t l;
      source++;  /* skip the `@' */
      bufflen -= sizeof(" '...' ");
      l = strlen(source);
      strcpy(out, "");
      if (l > bufflen) {
        source += (l-bufflen);  /* get last part of file name */
        strcat(out, "...");
      }
      strcat(out, source);
    }
    else {  /* out = [string "string"] */
      size_t len = strcspn(source, "\n\r");  /* stop at first newline */
      bufflen -= sizeof(" [string \"...\"] ");
      if (len > bufflen) len = bufflen;
      strcpy(out, "[string \"");
      if (source[len] != '\0') {  /* must truncate? */
        strncat(out, source, len);
        strcat(out, "...");
      }
      else
        strcat(out, source);
      strcat(out, "\"]");
    }
  }
}
