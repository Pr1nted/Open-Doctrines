#include "od9fmt.h"

typedef struct {
    char *buf;
    unsigned long cap;           /* bytes buf can take, NUL included */
    unsigned long len;           /* bytes that would have been written */
} out_t;

static void put(out_t *o, char c)
{
    if (o->cap && o->len < o->cap - 1) o->buf[o->len] = c;
    o->len++;
}

static void field(out_t *o, const char *s, int n, int width, int left, char pad)
{
    int k;
    if (!left)
        for (k = n; k < width; k++) put(o, pad);
    for (k = 0; k < n; k++) put(o, s[k]);
    if (left)
        for (k = n; k < width; k++) put(o, ' ');
}

int od9_vsnprintf(char *buf, unsigned long cap, const char *fmt, va_list ap)
{
    out_t o;
    char tmp[32];
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    while (*fmt) {
        int left = 0, width = 0, lng = 0, neg = 0, n;
        char pad = ' ', conv;
        unsigned long long v = 0;
        if (*fmt != '%') {
            put(&o, *fmt++);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        conv = *fmt;
        if (!conv) break;
        fmt++;
        if (left) pad = ' ';
        switch (conv) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            for (n = 0; s[n]; n++)
                ;
            field(&o, s, n, width, left, ' ');
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            field(&o, tmp, 1, width, left, ' ');
            break;
        case 'd':
        case 'i': {
            long long sv = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            if (sv < 0) {
                neg = 1;
                v = (unsigned long long)0 - (unsigned long long)sv;
            } else
                v = (unsigned long long)sv;
            goto number;
        }
        case 'u':
        case 'x':
            v = lng >= 2 ? va_arg(ap, unsigned long long)
                : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
        number: {
            unsigned base = conv == 'x' ? 16 : 10;
            char digits[24];
            int k = 0, i;
            do {
                digits[k++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v);
            n = 0;
            if (neg) tmp[n++] = '-';
            if (neg && pad == '0') {     /* the sign goes before the zeros */
                put(&o, '-');
                n = 0;
                width--;
            }
            for (i = k - 1; i >= 0; i--) tmp[n++] = digits[i];
            field(&o, tmp, n, width, left, pad);
            break;
        }
        case '%':
            put(&o, '%');
            break;
        default:                         /* not ours: print it as written */
            put(&o, '%');
            put(&o, conv);
            break;
        }
    }
    if (o.cap) o.buf[o.len < o.cap ? o.len : o.cap - 1] = 0;
    return (int)o.len;
}

int od9_snprintf(char *buf, unsigned long n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = od9_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* Only ever given a 32-byte buffer by the core (money()); bounded anyway. */
int od9_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = od9_vsnprintf(buf, 160, fmt, ap);
    va_end(ap);
    return r;
}
