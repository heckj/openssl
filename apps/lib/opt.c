/*
 * Copyright 2015-2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * This file is also used by the test suite. Do not #include "apps.h".
 */
#include "opt.h"
#include "fmt.h"
#include "internal/nelem.h"
#include <string.h>
#if !defined(OPENSSL_SYS_MSDOS)
# include <unistd.h>
#endif

#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <openssl/bio.h>
#include <openssl/x509v3.h>

#define MAX_OPT_HELP_WIDTH 30
const char OPT_HELP_STR[] = "-H";
const char OPT_MORE_STR[] = "-M";
const char OPT_SECTION_STR[] = "-S";
const char OPT_PARAM_STR[] = "-P";

/* Our state */
static char **argv;
static int argc;
static int opt_index;
static char *arg;
static char *flag;
static char *dunno;
static const OPTIONS *unknown;
static const OPTIONS *opts;
static char prog[40];

/*
 * Return the simple name of the program; removing various platform gunk.
 */
#if defined(OPENSSL_SYS_WIN32)

const char *opt_path_end(const char *filename)
{
    const char *p;

    /* find the last '/', '\' or ':' */
    for (p = filename + strlen(filename); --p > filename; )
        if (*p == '/' || *p == '\\' || *p == ':') {
            p++;
            break;
        }
    return p;
}

char *opt_progname(const char *argv0)
{
    size_t i, n;
    const char *p;
    char *q;

    p = opt_path_end(argv0);

    /* Strip off trailing nonsense. */
    n = strlen(p);
    if (n > 4 &&
        (strcmp(&p[n - 4], ".exe") == 0 || strcmp(&p[n - 4], ".EXE") == 0))
        n -= 4;

    /* Copy over the name, in lowercase. */
    if (n > sizeof(prog) - 1)
        n = sizeof(prog) - 1;
    for (q = prog, i = 0; i < n; i++, p++)
        *q++ = tolower((unsigned char)*p);
    *q = '\0';
    return prog;
}

#elif defined(OPENSSL_SYS_VMS)

const char *opt_path_end(const char *filename)
{
    const char *p;

    /* Find last special character sys:[foo.bar]openssl */
    for (p = filename + strlen(filename); --p > filename;)
        if (*p == ':' || *p == ']' || *p == '>') {
            p++;
            break;
        }
    return p;
}

char *opt_progname(const char *argv0)
{
    const char *p, *q;

    /* Find last special character sys:[foo.bar]openssl */
    p = opt_path_end(argv0);
    q = strrchr(p, '.');
    strncpy(prog, p, sizeof(prog) - 1);
    prog[sizeof(prog) - 1] = '\0';
    if (q != NULL && q - p < sizeof(prog))
        prog[q - p] = '\0';
    return prog;
}

#else

const char *opt_path_end(const char *filename)
{
    const char *p;

    /* Could use strchr, but this is like the ones above. */
    for (p = filename + strlen(filename); --p > filename;)
        if (*p == '/') {
            p++;
            break;
        }
    return p;
}

char *opt_progname(const char *argv0)
{
    const char *p;

    p = opt_path_end(argv0);
    strncpy(prog, p, sizeof(prog) - 1);
    prog[sizeof(prog) - 1] = '\0';
    return prog;
}
#endif

char *opt_appname(const char *arg0)
{
    size_t len = strlen(prog);

    if (arg0 != NULL)
        snprintf(prog + len, sizeof(prog) - len - 1, " %s", arg0);
    return prog;
}

char *opt_getprog(void)
{
    return prog;
}

/* Set up the arg parsing. */
char *opt_init(int ac, char **av, const OPTIONS *o)
{
    /* Store state. */
    argc = ac;
    argv = av;
    opt_begin();
    opts = o;
    unknown = NULL;

    /* Check all options up until the PARAM marker (if present) */
    for (; o->name != NULL && o->name != OPT_PARAM_STR; ++o) {
#ifndef NDEBUG
        const OPTIONS *next;
        int duplicated, i;
#endif

        if (o->name == OPT_HELP_STR
                || o->name == OPT_MORE_STR
                || o->name == OPT_SECTION_STR)
            continue;
#ifndef NDEBUG
        i = o->valtype;

        /* Make sure options are legit. */
        OPENSSL_assert(o->name[0] != '-');
        OPENSSL_assert(o->retval > 0);
        switch (i) {
        case   0: case '-': case '/': case '<': case '>': case 'E': case 'F':
        case 'M': case 'U': case 'f': case 'l': case 'n': case 'p': case 's':
        case 'u': case 'c': case ':':
            break;
        default:
            OPENSSL_assert(0);
        }

        /* Make sure there are no duplicates. */
        for (next = o + 1; next->name; ++next) {
            /*
             * Some compilers inline strcmp and the assert string is too long.
             */
            duplicated = strcmp(o->name, next->name) == 0;
            OPENSSL_assert(!duplicated);
        }
#endif
        if (o->name[0] == '\0') {
            OPENSSL_assert(unknown == NULL);
            unknown = o;
            OPENSSL_assert(unknown->valtype == 0 || unknown->valtype == '-');
        }
    }
    return prog;
}

static OPT_PAIR formats[] = {
    {"PEM/DER", OPT_FMT_PEMDER},
    {"pkcs12", OPT_FMT_PKCS12},
    {"smime", OPT_FMT_SMIME},
    {"engine", OPT_FMT_ENGINE},
    {"msblob", OPT_FMT_MSBLOB},
    {"nss", OPT_FMT_NSS},
    {"text", OPT_FMT_TEXT},
    {"http", OPT_FMT_HTTP},
    {"pvk", OPT_FMT_PVK},
    {NULL}
};

/* Print an error message about a failed format parse. */
int opt_format_error(const char *s, unsigned long flags)
{
    OPT_PAIR *ap;

    if (flags == OPT_FMT_PEMDER) {
        opt_printf_stderr("%s: Bad format \"%s\"; must be pem or der\n",
                          prog, s);
    } else {
        opt_printf_stderr("%s: Bad format \"%s\"; must be one of:\n",
                          prog, s);
        for (ap = formats; ap->name; ap++)
            if (flags & ap->retval)
                opt_printf_stderr("   %s\n", ap->name);
    }
    return 0;
}

/* Parse a format string, put it into *result; return 0 on failure, else 1. */
int opt_format(const char *s, unsigned long flags, int *result)
{
    switch (*s) {
    default:
        opt_printf_stderr("%s: Bad format \"%s\"\n", prog, s);
        return 0;
    case 'D':
    case 'd':
        if ((flags & OPT_FMT_PEMDER) == 0)
            return opt_format_error(s, flags);
        *result = FORMAT_ASN1;
        break;
    case 'T':
    case 't':
        if ((flags & OPT_FMT_TEXT) == 0)
            return opt_format_error(s, flags);
        *result = FORMAT_TEXT;
        break;
    case 'N':
    case 'n':
        if ((flags & OPT_FMT_NSS) == 0)
            return opt_format_error(s, flags);
        if (strcmp(s, "NSS") != 0 && strcmp(s, "nss") != 0)
            return opt_format_error(s, flags);
        *result = FORMAT_NSS;
        break;
    case 'S':
    case 's':
        if ((flags & OPT_FMT_SMIME) == 0)
            return opt_format_error(s, flags);
        *result = FORMAT_SMIME;
        break;
    case 'M':
    case 'm':
        if ((flags & OPT_FMT_MSBLOB) == 0)
            return opt_format_error(s, flags);
        *result = FORMAT_MSBLOB;
        break;
    case 'E':
    case 'e':
        if ((flags & OPT_FMT_ENGINE) == 0)
            return opt_format_error(s, flags);
        *result = FORMAT_ENGINE;
        break;
    case 'H':
    case 'h':
        if ((flags & OPT_FMT_HTTP) == 0)
            return opt_format_error(s, flags);
        *result = FORMAT_HTTP;
        break;
    case '1':
        if ((flags & OPT_FMT_PKCS12) == 0)
            return opt_format_error(s, flags);
        *result = FORMAT_PKCS12;
        break;
    case 'P':
    case 'p':
        if (s[1] == '\0' || strcmp(s, "PEM") == 0 || strcmp(s, "pem") == 0) {
            if ((flags & OPT_FMT_PEMDER) == 0)
                return opt_format_error(s, flags);
            *result = FORMAT_PEM;
        } else if (strcmp(s, "PVK") == 0 || strcmp(s, "pvk") == 0) {
            if ((flags & OPT_FMT_PVK) == 0)
                return opt_format_error(s, flags);
            *result = FORMAT_PVK;
        } else if (strcmp(s, "P12") == 0 || strcmp(s, "p12") == 0
                   || strcmp(s, "PKCS12") == 0 || strcmp(s, "pkcs12") == 0) {
            if ((flags & OPT_FMT_PKCS12) == 0)
                return opt_format_error(s, flags);
            *result = FORMAT_PKCS12;
        } else {
            opt_printf_stderr("%s: Bad format \"%s\"\n", prog, s);
            return 0;
        }
        break;
    }
    return 1;
}

/* Return string representing the given format. */
const char *format2str(int format)
{
    switch (format) {
    default:
        return "(undefined)";
    case FORMAT_PEM:
        return "PEM";
    case FORMAT_ASN1:
        return "DER";
    case FORMAT_TEXT:
        return "TEXT";
    case FORMAT_NSS:
        return "NSS";
    case FORMAT_SMIME:
        return "SMIME";
    case FORMAT_MSBLOB:
        return "MSBLOB";
    case FORMAT_ENGINE:
        return "ENGINE";
    case FORMAT_HTTP:
        return "HTTP";
    case FORMAT_PKCS12:
        return "P12";
    case FORMAT_PVK:
        return "PVK";
    }
}

/* Print an error message about unsuitable/unsupported format requested. */
void print_format_error(int format, unsigned long flags)
{
    (void)opt_format_error(format2str(format), flags);
}

/* Parse a cipher name, put it in *EVP_CIPHER; return 0 on failure, else 1. */
int opt_cipher(const char *name, const EVP_CIPHER **cipherp)
{
    *cipherp = EVP_get_cipherbyname(name);
    if (*cipherp != NULL)
        return 1;
    opt_printf_stderr("%s: Unknown cipher: %s\n", prog, name);
    return 0;
}

/*
 * Parse message digest name, put it in *EVP_MD; return 0 on failure, else 1.
 */
int opt_md(const char *name, const EVP_MD **mdp)
{
    *mdp = EVP_get_digestbyname(name);
    if (*mdp != NULL)
        return 1;
    opt_printf_stderr("%s: Unknown message digest: %s\n", prog, name);
    return 0;
}

/* Look through a list of name/value pairs. */
int opt_pair(const char *name, const OPT_PAIR* pairs, int *result)
{
    const OPT_PAIR *pp;

    for (pp = pairs; pp->name; pp++)
        if (strcmp(pp->name, name) == 0) {
            *result = pp->retval;
            return 1;
        }
    opt_printf_stderr("%s: Value must be one of:\n", prog);
    for (pp = pairs; pp->name; pp++)
        opt_printf_stderr("\t%s\n", pp->name);
    return 0;
}

/* Parse an int, put it into *result; return 0 on failure, else 1. */
int opt_int(const char *value, int *result)
{
    long l;

    if (!opt_long(value, &l))
        return 0;
    *result = (int)l;
    if (*result != l) {
        opt_printf_stderr("%s: Value \"%s\" outside integer range\n",
                          prog, value);
        return 0;
    }
    return 1;
}

static void opt_number_error(const char *v)
{
    size_t i = 0;
    struct strstr_pair_st {
        char *prefix;
        char *name;
    } b[] = {
        {"0x", "a hexadecimal"},
        {"0X", "a hexadecimal"},
        {"0", "an octal"}
    };

    for (i = 0; i < OSSL_NELEM(b); i++) {
        if (strncmp(v, b[i].prefix, strlen(b[i].prefix)) == 0) {
            opt_printf_stderr("%s: Can't parse \"%s\" as %s number\n",
                              prog, v, b[i].name);
            return;
        }
    }
    opt_printf_stderr("%s: Can't parse \"%s\" as a number\n", prog, v);
    return;
}

/* Parse a long, put it into *result; return 0 on failure, else 1. */
int opt_long(const char *value, long *result)
{
    int oerrno = errno;
    long l;
    char *endp;

    errno = 0;
    l = strtol(value, &endp, 0);
    if (*endp
            || endp == value
            || ((l == LONG_MAX || l == LONG_MIN) && errno == ERANGE)
            || (l == 0 && errno != 0)) {
        opt_number_error(value);
        errno = oerrno;
        return 0;
    }
    *result = l;
    errno = oerrno;
    return 1;
}

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L && \
    defined(INTMAX_MAX) && defined(UINTMAX_MAX) && \
    !defined(OPENSSL_NO_INTTYPES_H)

/* Parse an intmax_t, put it into *result; return 0 on failure, else 1. */
int opt_imax(const char *value, intmax_t *result)
{
    int oerrno = errno;
    intmax_t m;
    char *endp;

    errno = 0;
    m = strtoimax(value, &endp, 0);
    if (*endp
            || endp == value
            || ((m == INTMAX_MAX || m == INTMAX_MIN) && errno == ERANGE)
            || (m == 0 && errno != 0)) {
        opt_number_error(value);
        errno = oerrno;
        return 0;
    }
    *result = m;
    errno = oerrno;
    return 1;
}

/* Parse a uintmax_t, put it into *result; return 0 on failure, else 1. */
int opt_umax(const char *value, uintmax_t *result)
{
    int oerrno = errno;
    uintmax_t m;
    char *endp;

    errno = 0;
    m = strtoumax(value, &endp, 0);
    if (*endp
            || endp == value
            || (m == UINTMAX_MAX && errno == ERANGE)
            || (m == 0 && errno != 0)) {
        opt_number_error(value);
        errno = oerrno;
        return 0;
    }
    *result = m;
    errno = oerrno;
    return 1;
}
#endif

/*
 * Parse an unsigned long, put it into *result; return 0 on failure, else 1.
 */
int opt_ulong(const char *value, unsigned long *result)
{
    int oerrno = errno;
    char *endptr;
    unsigned long l;

    errno = 0;
    l = strtoul(value, &endptr, 0);
    if (*endptr
            || endptr == value
            || ((l == ULONG_MAX) && errno == ERANGE)
            || (l == 0 && errno != 0)) {
        opt_number_error(value);
        errno = oerrno;
        return 0;
    }
    *result = l;
    errno = oerrno;
    return 1;
}

/*
 * We pass opt as an int but cast it to "enum range" so that all the
 * items in the OPT_V_ENUM enumeration are caught; this makes -Wswitch
 * in gcc do the right thing.
 */
enum range { OPT_V_ENUM };

int opt_verify(int opt, X509_VERIFY_PARAM *vpm)
{
    int i;
    ossl_intmax_t t = 0;
    ASN1_OBJECT *otmp;
    X509_PURPOSE *xptmp;
    const X509_VERIFY_PARAM *vtmp;

    OPENSSL_assert(vpm != NULL);
    OPENSSL_assert(opt > OPT_V__FIRST);
    OPENSSL_assert(opt < OPT_V__LAST);

    switch ((enum range)opt) {
    case OPT_V__FIRST:
    case OPT_V__LAST:
        return 0;
    case OPT_V_POLICY:
        otmp = OBJ_txt2obj(opt_arg(), 0);
        if (otmp == NULL) {
            opt_printf_stderr("%s: Invalid Policy %s\n", prog, opt_arg());
            return 0;
        }
        X509_VERIFY_PARAM_add0_policy(vpm, otmp);
        break;
    case OPT_V_PURPOSE:
        /* purpose name -> purpose index */
        i = X509_PURPOSE_get_by_sname(opt_arg());
        if (i < 0) {
            opt_printf_stderr("%s: Invalid purpose %s\n", prog, opt_arg());
            return 0;
        }

        /* purpose index -> purpose object */
        xptmp = X509_PURPOSE_get0(i);

        /* purpose object -> purpose value */
        i = X509_PURPOSE_get_id(xptmp);

        if (!X509_VERIFY_PARAM_set_purpose(vpm, i)) {
            opt_printf_stderr("%s: Internal error setting purpose %s\n",
                              prog, opt_arg());
            return 0;
        }
        break;
    case OPT_V_VERIFY_NAME:
        vtmp = X509_VERIFY_PARAM_lookup(opt_arg());
        if (vtmp == NULL) {
            opt_printf_stderr("%s: Invalid verify name %s\n",
                              prog, opt_arg());
            return 0;
        }
        X509_VERIFY_PARAM_set1(vpm, vtmp);
        break;
    case OPT_V_VERIFY_DEPTH:
        i = atoi(opt_arg());
        if (i >= 0)
            X509_VERIFY_PARAM_set_depth(vpm, i);
        break;
    case OPT_V_VERIFY_AUTH_LEVEL:
        i = atoi(opt_arg());
        if (i >= 0)
            X509_VERIFY_PARAM_set_auth_level(vpm, i);
        break;
    case OPT_V_ATTIME:
        if (!opt_imax(opt_arg(), &t))
            return 0;
        if (t != (time_t)t) {
            opt_printf_stderr("%s: epoch time out of range %s\n",
                              prog, opt_arg());
            return 0;
        }
        X509_VERIFY_PARAM_set_time(vpm, (time_t)t);
        break;
    case OPT_V_VERIFY_HOSTNAME:
        if (!X509_VERIFY_PARAM_set1_host(vpm, opt_arg(), 0))
            return 0;
        break;
    case OPT_V_VERIFY_EMAIL:
        if (!X509_VERIFY_PARAM_set1_email(vpm, opt_arg(), 0))
            return 0;
        break;
    case OPT_V_VERIFY_IP:
        if (!X509_VERIFY_PARAM_set1_ip_asc(vpm, opt_arg()))
            return 0;
        break;
    case OPT_V_IGNORE_CRITICAL:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_IGNORE_CRITICAL);
        break;
    case OPT_V_ISSUER_CHECKS:
        /* NOP, deprecated */
        break;
    case OPT_V_CRL_CHECK:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_CRL_CHECK);
        break;
    case OPT_V_CRL_CHECK_ALL:
        X509_VERIFY_PARAM_set_flags(vpm,
                                    X509_V_FLAG_CRL_CHECK |
                                    X509_V_FLAG_CRL_CHECK_ALL);
        break;
    case OPT_V_POLICY_CHECK:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_POLICY_CHECK);
        break;
    case OPT_V_EXPLICIT_POLICY:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_EXPLICIT_POLICY);
        break;
    case OPT_V_INHIBIT_ANY:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_INHIBIT_ANY);
        break;
    case OPT_V_INHIBIT_MAP:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_INHIBIT_MAP);
        break;
    case OPT_V_X509_STRICT:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_X509_STRICT);
        break;
    case OPT_V_EXTENDED_CRL:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_EXTENDED_CRL_SUPPORT);
        break;
    case OPT_V_USE_DELTAS:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_USE_DELTAS);
        break;
    case OPT_V_POLICY_PRINT:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_NOTIFY_POLICY);
        break;
    case OPT_V_CHECK_SS_SIG:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_CHECK_SS_SIGNATURE);
        break;
    case OPT_V_TRUSTED_FIRST:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_TRUSTED_FIRST);
        break;
    case OPT_V_SUITEB_128_ONLY:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_SUITEB_128_LOS_ONLY);
        break;
    case OPT_V_SUITEB_128:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_SUITEB_128_LOS);
        break;
    case OPT_V_SUITEB_192:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_SUITEB_192_LOS);
        break;
    case OPT_V_PARTIAL_CHAIN:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_PARTIAL_CHAIN);
        break;
    case OPT_V_NO_ALT_CHAINS:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_NO_ALT_CHAINS);
        break;
    case OPT_V_NO_CHECK_TIME:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_NO_CHECK_TIME);
        break;
    case OPT_V_ALLOW_PROXY_CERTS:
        X509_VERIFY_PARAM_set_flags(vpm, X509_V_FLAG_ALLOW_PROXY_CERTS);
        break;
    }
    return 1;

}

void opt_begin(void)
{
    opt_index = 1;
    arg = NULL;
    flag = NULL;
}

/*
 * Parse the next flag (and value if specified), return 0 if done, -1 on
 * error, otherwise the flag's retval.
 */
int opt_next(void)
{
    char *p;
    const OPTIONS *o;
    int ival;
    long lval;
    unsigned long ulval;
    ossl_intmax_t imval;
    ossl_uintmax_t umval;

    /* Look at current arg; at end of the list? */
    arg = NULL;
    p = argv[opt_index];
    if (p == NULL)
        return 0;

    /* If word doesn't start with a -, we're done. */
    if (*p != '-')
        return 0;

    /* Hit "--" ? We're done. */
    opt_index++;
    if (strcmp(p, "--") == 0)
        return 0;

    /* Allow -nnn and --nnn */
    if (*++p == '-')
        p++;
    flag = p - 1;

    /* If we have --flag=foo, snip it off */
    if ((arg = strchr(p, '=')) != NULL)
        *arg++ = '\0';
    for (o = opts; o->name; ++o) {
        /* If not this option, move on to the next one. */
        if (!(strcmp(p, "h") == 0 && strcmp(o->name, "help") == 0)
                && strcmp(p, o->name) != 0)
            continue;

        /* If it doesn't take a value, make sure none was given. */
        if (o->valtype == 0 || o->valtype == '-') {
            if (arg) {
                opt_printf_stderr("%s: Option -%s does not take a value\n",
                                  prog, p);
                return -1;
            }
            return o->retval;
        }

        /* Want a value; get the next param if =foo not used. */
        if (arg == NULL) {
            if (argv[opt_index] == NULL) {
                opt_printf_stderr("%s: Option -%s needs a value\n",
                                  prog, o->name);
                return -1;
            }
            arg = argv[opt_index++];
        }

        /* Syntax-check value. */
        switch (o->valtype) {
        default:
        case 's':
        case ':':
            /* Just a string. */
            break;
        case '/':
            if (opt_isdir(arg) > 0)
                break;
            opt_printf_stderr("%s: Not a directory: %s\n", prog, arg);
            return -1;
        case '<':
            /* Input file. */
            break;
        case '>':
            /* Output file. */
            break;
        case 'p':
        case 'n':
            if (!opt_int(arg, &ival))
                return -1;
            if (o->valtype == 'p' && ival <= 0) {
                opt_printf_stderr("%s: Non-positive number \"%s\" for -%s\n",
                                  prog, arg, o->name);
                return -1;
            }
            break;
        case 'M':
            if (!opt_imax(arg, &imval))
                return -1;
            break;
        case 'U':
            if (!opt_umax(arg, &umval))
                return -1;
            break;
        case 'l':
            if (!opt_long(arg, &lval))
                return -1;
            break;
        case 'u':
            if (!opt_ulong(arg, &ulval))
                return -1;
            break;
        case 'c':
        case 'E':
        case 'F':
        case 'f':
            if (opt_format(arg,
                           o->valtype == 'c' ? OPT_FMT_PDS :
                           o->valtype == 'E' ? OPT_FMT_PDE :
                           o->valtype == 'F' ? OPT_FMT_PEMDER
                           : OPT_FMT_ANY, &ival))
                break;
            opt_printf_stderr("%s: Invalid format \"%s\" for -%s\n",
                              prog, arg, o->name);
            return -1;
        }

        /* Return the flag value. */
        return o->retval;
    }
    if (unknown != NULL) {
        dunno = p;
        return unknown->retval;
    }
    opt_printf_stderr("%s: Unknown option: -%s\n", prog, p);
    return -1;
}

/* Return the most recent flag parameter. */
char *opt_arg(void)
{
    return arg;
}

/* Return the most recent flag. */
char *opt_flag(void)
{
    return flag;
}

/* Return the unknown option. */
char *opt_unknown(void)
{
    return dunno;
}

/* Return the rest of the arguments after parsing flags. */
char **opt_rest(void)
{
    return &argv[opt_index];
}

/* How many items in remaining args? */
int opt_num_rest(void)
{
    int i = 0;
    char **pp;

    for (pp = opt_rest(); *pp; pp++, i++)
        continue;
    return i;
}

/* Return a string describing the parameter type. */
static const char *valtype2param(const OPTIONS *o)
{
    switch (o->valtype) {
    case 0:
    case '-':
        return "";
    case ':':
        return "uri";
    case 's':
        return "val";
    case '/':
        return "dir";
    case '<':
        return "infile";
    case '>':
        return "outfile";
    case 'p':
        return "+int";
    case 'n':
        return "int";
    case 'l':
        return "long";
    case 'u':
        return "ulong";
    case 'E':
        return "PEM|DER|ENGINE";
    case 'F':
        return "PEM|DER";
    case 'f':
        return "format";
    case 'M':
        return "intmax";
    case 'U':
        return "uintmax";
    }
    return "parm";
}

void opt_print(const OPTIONS *o, int doingparams, int width)
{
    const char* help;
    char start[80 + 1];
    char *p;

        help = o->helpstr ? o->helpstr : "(No additional info)";
        if (o->name == OPT_HELP_STR) {
            opt_printf_stderr(help, prog);
            return;
        }
        if (o->name == OPT_SECTION_STR) {
            opt_printf_stderr("\n");
            opt_printf_stderr(help, prog);
            return;
        }
        if (o->name == OPT_PARAM_STR) {
            opt_printf_stderr("\nParameters:\n");
            return;
        }

        /* Pad out prefix */
        memset(start, ' ', sizeof(start) - 1);
        start[sizeof(start) - 1] = '\0';

        if (o->name == OPT_MORE_STR) {
            /* Continuation of previous line; pad and print. */
            start[width] = '\0';
            opt_printf_stderr("%s  %s\n", start, help);
            return;
        }

        /* Build up the "-flag [param]" part. */
        p = start;
        *p++ = ' ';
        if (!doingparams)
            *p++ = '-';
        if (o->name[0])
            p += strlen(strcpy(p, o->name));
        else
            *p++ = '*';
        if (o->valtype != '-') {
            *p++ = ' ';
            p += strlen(strcpy(p, valtype2param(o)));
        }
        *p = ' ';
        if ((int)(p - start) >= MAX_OPT_HELP_WIDTH) {
            *p = '\0';
            opt_printf_stderr("%s\n", start);
            memset(start, ' ', sizeof(start));
        }
        start[width] = '\0';
        opt_printf_stderr("%s  %s\n", start, help);
}

void opt_help(const OPTIONS *list)
{
    const OPTIONS *o;
    int i, sawparams = 0, width = 5;
    int standard_prolog;
    char start[80 + 1];

    /* Starts with its own help message? */
    standard_prolog = list[0].name != OPT_HELP_STR;

    /* Find the widest help. */
    for (o = list; o->name; o++) {
        if (o->name == OPT_MORE_STR)
            continue;
        i = 2 + (int)strlen(o->name);
        if (o->valtype != '-')
            i += 1 + strlen(valtype2param(o));
        if (i < MAX_OPT_HELP_WIDTH && i > width)
            width = i;
        OPENSSL_assert(i < (int)sizeof(start));
    }

    if (standard_prolog) {
        opt_printf_stderr("Usage: %s [options]\n", prog);
        if (list[0].name != OPT_SECTION_STR)
            opt_printf_stderr("Valid options are:\n", prog);
    }

    /* Now let's print. */
    for (o = list; o->name; o++) {
        if (o->name == OPT_PARAM_STR)
            sawparams = 1;
        opt_print(o, sawparams, width);
    }
}

/* opt_isdir section */
#ifdef _WIN32
# include <windows.h>
int opt_isdir(const char *name)
{
    DWORD attr;
# if defined(UNICODE) || defined(_UNICODE)
    size_t i, len_0 = strlen(name) + 1;
    WCHAR tempname[MAX_PATH];

    if (len_0 > MAX_PATH)
        return -1;

#  if !defined(_WIN32_WCE) || _WIN32_WCE>=101
    if (!MultiByteToWideChar(CP_ACP, 0, name, len_0, tempname, MAX_PATH))
#  endif
        for (i = 0; i < len_0; i++)
            tempname[i] = (WCHAR)name[i];

    attr = GetFileAttributes(tempname);
# else
    attr = GetFileAttributes(name);
# endif
    if (attr == INVALID_FILE_ATTRIBUTES)
        return -1;
    return ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
}
#else
# include <sys/stat.h>
# ifndef S_ISDIR
#  if defined(_S_IFMT) && defined(_S_IFDIR)
#   define S_ISDIR(a)   (((a) & _S_IFMT) == _S_IFDIR)
#  else
#   define S_ISDIR(a)   (((a) & S_IFMT) == S_IFDIR)
#  endif
# endif

int opt_isdir(const char *name)
{
# if defined(S_ISDIR)
    struct stat st;

    if (stat(name, &st) == 0)
        return S_ISDIR(st.st_mode);
    else
        return -1;
# else
    return -1;
# endif
}
#endif
