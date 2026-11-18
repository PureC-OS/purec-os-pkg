#include "pkgd.h"
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sha512.h"

#ifdef PKG_SIG_OPENSSL
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#else
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/ecc.h>
#endif

#define POINT_LEN 65
#define MAX_KEYS 128
#define MAX_REVOKED 128
#define MAX_MASTERS 32

struct kentry {
    char fp[41];
    unsigned char point[POINT_LEN];
    int official;
    char repo[160];
    char names[256];
};

struct keyring {
    long serial;
    int count;
    struct kentry keys[MAX_KEYS];
    int revoked_count;
    char revoked[MAX_REVOKED][41];
};

enum { KR_UNLOADED, KR_ABSENT, KR_VALID, KR_INVALID };

static struct keyring kr;
static int kr_state = KR_UNLOADED;
static int kr_threshold = 1;
static char kr_error[200];
static int legacy_warned;
static int revoked_warned;

static int hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int load_key(const char *path, unsigned char point[POINT_LEN])
{
    char *text = read_text_file(path, NULL);
    if (!text)
        return -1;
    size_t n = 0;
    while (text[n] && hex_value((unsigned char)text[n]) >= 0)
        n++;
    int ok = n == POINT_LEN * 2;
    if (ok)
        for (int i = 0; i < POINT_LEN; i++)
            point[i] = (unsigned char)(hex_value((unsigned char)text[i * 2]) * 16 + hex_value((unsigned char)text[i * 2 + 1]));
    free(text);
    return ok && point[0] == 0x04 ? 0 : -1;
}

static int digest_file(const char *path, unsigned char digest[PURE_SHA512_DIGEST_LEN])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    pure_sha512_ctx ctx;
    pure_sha512_init(&ctx);
    unsigned char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        pure_sha512_update(&ctx, buf, got);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    pure_sha512_final(&ctx, digest);
    return 0;
}

static int verify_digest(const unsigned char *digest, const unsigned char *sig, size_t sig_len,
                         const unsigned char point[POINT_LEN])
{
#ifdef PKG_SIG_OPENSSL
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!key)
        return 0;
    const EC_GROUP *group = EC_KEY_get0_group(key);
    EC_POINT *pub = EC_POINT_new(group);
    int ok = 0;
    if (pub && EC_POINT_oct2point(group, pub, point, POINT_LEN, NULL) == 1 && EC_KEY_set_public_key(key, pub) == 1)
        ok = ECDSA_verify(0, digest, PURE_SHA512_DIGEST_LEN, sig, (int)sig_len, key) == 1;
    EC_POINT_free(pub);
    EC_KEY_free(key);
    return ok;
#else
    ecc_key key;
    int res = 0;
    if (wc_ecc_init(&key) != 0)
        return 0;
    if (wc_ecc_import_x963_ex(point, POINT_LEN, &key, ECC_SECP256R1) == 0)
        if (wc_ecc_verify_hash(sig, (word32)sig_len, digest, PURE_SHA512_DIGEST_LEN, &res, &key) != 0)
            res = 0;
    wc_ecc_free(&key);
    return res == 1;
#endif
}

static void fingerprint_of(const unsigned char point[POINT_LEN], char out[41])
{
    unsigned char digest[PURE_SHA512_DIGEST_LEN];
    pure_sha512_ctx ctx;
    pure_sha512_init(&ctx);
    pure_sha512_update(&ctx, point, POINT_LEN);
    pure_sha512_final(&ctx, digest);
    for (int i = 0; i < 20; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static void digest_bytes(const void *data, size_t length, unsigned char digest[PURE_SHA512_DIGEST_LEN])
{
    pure_sha512_ctx ctx;
    pure_sha512_init(&ctx);
    pure_sha512_update(&ctx, data, length);
    pure_sha512_final(&ctx, digest);
}

static int decode_hex(const char *hex, size_t digits, unsigned char *out)
{
    if (digits % 2)
        return -1;
    for (size_t i = 0; i < digits; i += 2) {
        int hi = hex_value((unsigned char)hex[i]), lo = hex_value((unsigned char)hex[i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i / 2] = (unsigned char)(hi * 16 + lo);
    }
    return 0;
}

int sig_fingerprint_file(const char *pub, char out[41])
{
    unsigned char point[POINT_LEN];
    if (load_key(pub, point) < 0)
        return -1;
    fingerprint_of(point, out);
    return 0;
}

static int revoked_fp(const char *fp)
{
    if (kr_state != KR_VALID)
        return 0;
    for (int i = 0; i < kr.revoked_count; i++)
        if (!strcmp(kr.revoked[i], fp))
            return 1;
    return 0;
}

static void scan_keys(const char *dir, const unsigned char *digest, const char *sig, size_t sig_len, int *keys,
                      int *valid, char signer[41])
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && !*valid) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".pub") != 0)
            continue;
        char path[PKG_PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unsigned char point[POINT_LEN];
        if (load_key(path, point) < 0)
            continue;
        char fp[41];
        fingerprint_of(point, fp);
        if (revoked_fp(fp))
            continue;
        (*keys)++;
        if (verify_digest(digest, (const unsigned char *)sig, sig_len, point)) {
            *valid = 1;
            memcpy(signer, fp, 41);
        }
    }
    closedir(d);
}

static void scan_dir_with_fallback(const char *relative, const unsigned char *digest, const char *sig,
                                   size_t sig_len, int *keys, int *valid, char signer[41])
{
    char target[PKG_PATH_MAX + 64], fallback[PKG_PATH_MAX];
    snprintf(target, sizeof(target), "%s", rp(relative));
    snprintf(fallback, sizeof(fallback), "/%s", relative);
    scan_keys(target, digest, sig, sig_len, keys, valid, signer);
    if (!*valid && strcmp(target, fallback) != 0)
        scan_keys(fallback, digest, sig, sig_len, keys, valid, signer);
}

struct master {
    char fp[41];
    unsigned char point[POINT_LEN];
};

static int load_masters_dir(const char *dir, struct master *out, int count)
{
    DIR *d = opendir(dir);
    if (!d)
        return count;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < MAX_MASTERS) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".pub") != 0)
            continue;
        char path[PKG_PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (load_key(path, out[count].point) < 0)
            continue;
        fingerprint_of(out[count].point, out[count].fp);
        count++;
    }
    closedir(d);
    return count;
}

static int load_masters(struct master *out)
{
    char target[PKG_PATH_MAX + 64];
    snprintf(target, sizeof(target), "%s", rp("etc/pkg/keys/master"));
    int count = load_masters_dir(target, out, 0);
    if (!count && strcmp(target, "/etc/pkg/keys/master") != 0)
        count = load_masters_dir("/etc/pkg/keys/master", out, 0);
    return count;
}

static void seterr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void seterr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(kr_error, sizeof(kr_error), fmt, ap);
    va_end(ap);
}

static int keyring_parse(const char *text, size_t length, struct keyring *out)
{
    memset(out, 0, sizeof(*out));
    size_t payload = length;
    int have_serial = 0;
    for (size_t pos = 0; pos < length;) {
        const char *eol = memchr(text + pos, '\n', length - pos);
        size_t line_len = eol ? (size_t)(eol - (text + pos)) : length - pos;
        if (line_len >= 5 && !strncmp(text + pos, "#sig ", 5)) {
            payload = pos;
            break;
        }
        pos += line_len + (eol ? 1 : 0);
    }
    char *copy = malloc(payload + 1);
    if (!copy) {
        seterr("out of memory");
        return -1;
    }
    memcpy(copy, text, payload);
    copy[payload] = '\0';
    int lineno = 0;
    for (char *line = copy; line && *line;) {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';
        lineno++;
        char *next = eol ? eol + 1 : NULL;
        char *save = NULL;
        char *word = strtok_r(line, " \t\r", &save);
        line = next;
        if (!word || *word == '#')
            continue;
        if (!strcmp(word, "serial")) {
            char *v = strtok_r(NULL, " \t\r", &save);
            if (!v || !*v) {
                seterr("line %d: serial without a number", lineno);
                free(copy);
                return -1;
            }
            out->serial = strtol(v, NULL, 10);
            have_serial = 1;
        } else if (!strcmp(word, "revoke")) {
            char *fp = strtok_r(NULL, " \t\r", &save);
            if (!fp || strlen(fp) != 40 || out->revoked_count == MAX_REVOKED) {
                seterr("line %d: bad revoke entry", lineno);
                free(copy);
                return -1;
            }
            snprintf(out->revoked[out->revoked_count++], 41, "%s", fp);
        } else if (!strcmp(word, "key")) {
            char *fp = strtok_r(NULL, " \t\r", &save);
            char *hex = strtok_r(NULL, " \t\r", &save);
            if (!fp || !hex || out->count == MAX_KEYS || strlen(fp) != 40 || strlen(hex) != POINT_LEN * 2) {
                seterr("line %d: bad key entry", lineno);
                free(copy);
                return -1;
            }
            struct kentry *k = &out->keys[out->count];
            memset(k, 0, sizeof(*k));
            char check[41];
            if (decode_hex(hex, POINT_LEN * 2, k->point) < 0 || k->point[0] != 0x04) {
                seterr("line %d: the key is not a P-256 point", lineno);
                free(copy);
                return -1;
            }
            fingerprint_of(k->point, check);
            if (strcmp(check, fp) != 0) {
                seterr("line %d: the fingerprint does not match the key", lineno);
                free(copy);
                return -1;
            }
            snprintf(k->fp, sizeof(k->fp), "%s", check);
            char *attr;
            int scoped = 0;
            while ((attr = strtok_r(NULL, " \t\r", &save)) != NULL) {
                if (!strcmp(attr, "official"))
                    k->official = 1;
                else if (!strncmp(attr, "repo=", 5)) {
                    snprintf(k->repo, sizeof(k->repo), "%s", attr + 5);
                    scoped = 1;
                } else if (!strncmp(attr, "names=", 6))
                    snprintf(k->names, sizeof(k->names), "%s", attr + 6);
            }
            if (k->official == scoped || (scoped && !k->names[0])) {
                seterr("line %d: a key is either 'official' or 'repo=URL names=GLOBS'", lineno);
                free(copy);
                return -1;
            }
            out->count++;
        }
    }
    free(copy);
    if (!have_serial) {
        seterr("no serial line");
        return -1;
    }
    struct master masters[MAX_MASTERS];
    int mcount = load_masters(masters);
    if (!mcount) {
        seterr("no master keys in %s", rp("etc/pkg/keys/master"));
        return -1;
    }
    unsigned char digest[PURE_SHA512_DIGEST_LEN];
    digest_bytes(text, payload, digest);
    char seen[MAX_MASTERS][41];
    int distinct = 0;
    for (size_t pos = payload; pos < length;) {
        const char *eol = memchr(text + pos, '\n', length - pos);
        size_t line_len = eol ? (size_t)(eol - (text + pos)) : length - pos;
        const char *line = text + pos;
        pos += line_len + (eol ? 1 : 0);
        if (line_len < 5 || strncmp(line, "#sig ", 5) != 0)
            continue;
        char fp[41];
        size_t i = 5;
        size_t n = 0;
        while (i < line_len && line[i] != ' ' && n < 40)
            fp[n++] = line[i++];
        fp[n] = '\0';
        if (n != 40 || i >= line_len || line[i] != ' ')
            continue;
        i++;
        size_t digits = line_len - i;
        while (digits && (line[i + digits - 1] == '\r' || line[i + digits - 1] == ' '))
            digits--;
        unsigned char der[128];
        if (digits == 0 || digits / 2 > sizeof(der) || decode_hex(line + i, digits, der) < 0)
            continue;
        int dup = 0;
        for (int s = 0; s < distinct; s++)
            if (!strcmp(seen[s], fp))
                dup = 1;
        if (dup)
            continue;
        for (int m = 0; m < mcount; m++) {
            if (strcmp(masters[m].fp, fp) != 0)
                continue;
            if (verify_digest(digest, der, digits / 2, masters[m].point)) {
                snprintf(seen[distinct++], 41, "%s", fp);
                if (distinct == MAX_MASTERS)
                    distinct--;
            }
            break;
        }
    }
    if (distinct < kr_threshold) {
        seterr("%d valid master signature(s), %d required", distinct, kr_threshold);
        return -1;
    }
    return 0;
}

static void keyring_load(void)
{
    if (kr_state != KR_UNLOADED)
        return;
    size_t length = 0;
    char *text = read_text_file(rp("etc/pkg/keyring.conf"), &length);
    if (!text) {
        kr_state = KR_ABSENT;
        return;
    }
    if (keyring_parse(text, length, &kr) == 0) {
        kr_state = KR_VALID;
    } else {
        kr_state = KR_INVALID;
        warn("%s is not valid: %s", rp("etc/pkg/keyring.conf"), kr_error);
    }
    free(text);
}

void sig_configure(int threshold)
{
    kr_threshold = threshold < 1 ? 1 : threshold;
    kr_state = KR_UNLOADED;
}

int sig_keyring_mode(void)
{
    keyring_load();
    return kr_state == KR_VALID ? 1 : kr_state == KR_ABSENT ? 0 : -1;
}

const char *sig_keyring_error(void)
{
    return kr_error;
}

long sig_keyring_serial(void)
{
    keyring_load();
    return kr_state == KR_VALID ? kr.serial : -1;
}

int sig_keyring_install(const char *candidate, long *new_serial)
{
    size_t length = 0;
    char *text = read_text_file(candidate, &length);
    if (!text) {
        seterr("cannot read the downloaded keyring");
        return -1;
    }
    static struct keyring fresh;
    if (keyring_parse(text, length, &fresh) < 0) {
        free(text);
        return -1;
    }
    *new_serial = fresh.serial;
    keyring_load();
    if (kr_state == KR_VALID && fresh.serial < kr.serial) {
        free(text);
        seterr("serial %ld is older than the installed %ld", fresh.serial, kr.serial);
        return -2;
    }
    int same = 0;
    if (kr_state == KR_VALID && fresh.serial == kr.serial) {
        size_t old_length = 0;
        char *old_text = read_text_file(rp("etc/pkg/keyring.conf"), &old_length);
        same = old_text && old_length == length && memcmp(old_text, text, length) == 0;
        free(old_text);
    }
    mkdirs(rp("etc/pkg"), 0755);
    int rc = write_text_file(rp("etc/pkg/keyring.conf"), text, length);
    free(text);
    kr_state = KR_UNLOADED;
    if (rc < 0) {
        seterr("cannot write %s", rp("etc/pkg/keyring.conf"));
        return -1;
    }
    return same ? 1 : 0;
}

int sig_is_revoked(const char *fp)
{
    keyring_load();
    return fp && fp[0] && revoked_fp(fp);
}

void sig_warn_revoked_installed(void)
{
    keyring_load();
    if (kr_state != KR_VALID || revoked_warned)
        return;
    char list[512] = "";
    int n = 0;
    for (int i = 0; i < ldb.count; i++)
        if (ldb.pkgs[i].signer[0] && revoked_fp(ldb.pkgs[i].signer)) {
            size_t used = strlen(list);
            if (used + strlen(ldb.pkgs[i].name) + 2 < sizeof(list))
                snprintf(list + used, sizeof(list) - used, "%s%s", n ? ", " : "", ldb.pkgs[i].name);
            n++;
        }
    if (n) {
        revoked_warned = 1;
        warn("%d installed package(s) were signed with a revoked key: %s", n, list);
    }
}

static void normalize_repo(const char *url, char *out, size_t cap)
{
    const char *p = url;
    if (!strncmp(p, "https://", 8))
        p += 8;
    else if (!strncmp(p, "http://", 7))
        p += 7;
    if (!strncmp(p, "www.", 4))
        p += 4;
    size_t n = strlen(p);
    for (;;) {
        if (n && p[n - 1] == '/')
            n--;
        else if (n > 4 && !strncmp(p + n - 4, ".git", 4))
            n -= 4;
        else
            break;
    }
    if (n >= cap)
        n = cap - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
    out[n] = '\0';
}

static int names_match(const char *globs, const char *name)
{
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", globs);
    char *save = NULL;
    for (char *g = strtok_r(copy, ",", &save); g; g = strtok_r(NULL, ",", &save))
        if (fnmatch(g, name, 0) == 0)
            return 1;
    return 0;
}

static int eligible(const struct kentry *k, const struct sig_scope *scope)
{
    if (revoked_fp(k->fp))
        return 0;
    if (scope->kind != SIG_SCOPE_REPO)
        return k->official;
    if (k->official || !scope->repo || !scope->name)
        return 0;
    char a[200], b[200];
    normalize_repo(k->repo, a, sizeof(a));
    normalize_repo(scope->repo, b, sizeof(b));
    return !strcmp(a, b) && names_match(k->names, scope->name);
}

int sig_verify_file(const char *file, const char *signature_file, const struct sig_scope *scope, char signer_fp[41])
{
    signer_fp[0] = '\0';
    unsigned char digest[PURE_SHA512_DIGEST_LEN];
    if (digest_file(file, digest) < 0)
        return SIG_BAD;
    size_t sig_len = 0;
    char *sig = read_text_file(signature_file, &sig_len);
    if (!sig || sig_len == 0 || sig_len > 128) {
        free(sig);
        return SIG_BAD;
    }
    keyring_load();
    int keys = 0, valid = 0;
    if (kr_state == KR_INVALID) {
        free(sig);
        return SIG_BAD;
    }
    if (kr_state == KR_ABSENT) {
        if (!legacy_warned) {
            legacy_warned = 1;
            warn("no keyring.conf: the keys in %s are not bound to repositories", rp("etc/pkg/keys"));
        }
        char target_dir[PKG_PATH_MAX + 64];
        snprintf(target_dir, sizeof(target_dir), "%s", rp("etc/pkg/keys"));
        scan_keys(target_dir, digest, sig, sig_len, &keys, &valid, signer_fp);
        if (!valid && strcmp(target_dir, "/etc/pkg/keys") != 0)
            scan_keys("/etc/pkg/keys", digest, sig, sig_len, &keys, &valid, signer_fp);
    } else {
        for (int i = 0; i < kr.count && !valid; i++) {
            if (!eligible(&kr.keys[i], scope))
                continue;
            keys++;
            if (verify_digest(digest, (const unsigned char *)sig, sig_len, kr.keys[i].point)) {
                valid = 1;
                memcpy(signer_fp, kr.keys[i].fp, 41);
            }
        }
        if (!valid && scope->kind == SIG_SCOPE_LOCAL)
            scan_dir_with_fallback("etc/pkg/keys/local", digest, sig, sig_len, &keys, &valid, signer_fp);
    }
    free(sig);
    if (valid)
        return SIG_OK;
    return keys ? SIG_BAD : SIG_NOKEYS;
}
