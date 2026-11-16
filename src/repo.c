#include "json.h"
#include "pkgd.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_SOURCES 128
#define MAX_PER_REPO 32
#define SIG_NEVER 0
#define SIG_OPTIONAL 1
#define SIG_REQUIRED 2
#define REGISTRY_FILE "registry.conf"
#define REPOSITORY_FILE "repository.conf"

struct source {
    char url[PKG_PATH_MAX];
    char owner_repo[96];
    int local;
};

static struct source sources[MAX_SOURCES];
static int sources_total;
static int locals_ignored;
static char arch[16] = "x86_64";
static char api_base[PKG_PATH_MAX] = "https://api.github.com";
static char registry[PKG_PATH_MAX];
static int siglevel = SIG_OPTIONAL;
static int allow_prerelease;
static int allow_local;
static char config_used[PKG_PATH_MAX];

struct syncdb sdb;

int repo_count(void)
{
    return sources_total;
}

const char *config_path(void)
{
    return config_used;
}

static char *strip(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

static int yes(const char *v)
{
    return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
}

static int parse_siglevel(const char *value)
{
    if (!strcasecmp(value, "Never"))
        return SIG_NEVER;
    if (!strcasecmp(value, "Required"))
        return SIG_REQUIRED;
    return SIG_OPTIONAL;
}

static int parse_github(const char *url, char *out, size_t cap)
{
    const char *p = url;
    if (!strncmp(p, "https://", 8))
        p += 8;
    else if (!strncmp(p, "http://", 7))
        p += 7;
    if (!strncmp(p, "www.", 4))
        p += 4;
    if (strncmp(p, "github.com/", 11) != 0)
        return -1;
    p += 11;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p || !slash[1])
        return -1;
    size_t n = strlen(p);
    while (n && p[n - 1] == '/')
        n--;
    if (n > 4 && !strncmp(p + n - 4, ".git", 4))
        n -= 4;
    const char *extra = memchr(slash + 1, '/', n - (size_t)(slash + 1 - p));
    if (extra)
        n = (size_t)(extra - p);
    if (n + 1 > cap)
        return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int valid_name(const char *name)
{
    if (!*name || strlen(name) >= PKG_NAME_MAX)
        return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.' && *p != '+')
            return 0;
    return 1;
}

static int add_source(const char *url, int local, const char *where, int line)
{
    char owner_repo[96];
    if (parse_github(url, owner_repo, sizeof(owner_repo)) < 0) {
        warn("%s:%d: '%s' is not a GitHub repository URL, ignored", where, line, url);
        return -1;
    }
    for (int i = 0; i < sources_total; i++)
        if (!strcasecmp(sources[i].owner_repo, owner_repo))
            return 0;
    if (sources_total == MAX_SOURCES) {
        warn("%s: too many repositories, %s ignored", where, url);
        return -1;
    }
    struct source *s = &sources[sources_total++];
    memset(s, 0, sizeof(*s));
    snprintf(s->url, sizeof(s->url), "https://github.com/%s", owner_repo);
    snprintf(s->owner_repo, sizeof(s->owner_repo), "%s", owner_repo);
    s->local = local;
    return 0;
}

static char *value_of_line(char *s)
{
    char *eq = strchr(s, '=');
    return eq ? strip(eq + 1) : s;
}

static void load_registry_file(const char *path)
{
    char *text = read_text_file(path, NULL);
    if (!text)
        return;
    int lineno = 0;
    for (char *line = text; line && *line;) {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';
        lineno++;
        char *s = strip(line);
        if (*s && *s != '#')
            add_source(value_of_line(s), 0, "registry", lineno);
        line = eol ? eol + 1 : NULL;
    }
    free(text);
}

static int load_file(const char *path, int required)
{
    char *text = read_text_file(path, NULL);
    if (!text) {
        if (required)
            die("cannot read %s", path);
        return -1;
    }
    snprintf(config_used, sizeof(config_used), "%s", path);
    enum { NONE, OPTIONS, TRUSTED } section = NONE;
    int lineno = 0;
    static char locals[MAX_SOURCES][PKG_PATH_MAX];
    int local_lines[MAX_SOURCES];
    int local_count = 0;
    char *line = text;
    while (*line) {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';
        lineno++;
        char *s = strip(line);
        if (*s && *s != '#') {
            if (*s == '[') {
                char *end = strchr(s, ']');
                if (!end)
                    die("%s:%d: malformed section '%s'", path, lineno, s);
                *end = '\0';
                if (!strcmp(s + 1, "options"))
                    section = OPTIONS;
                else if (!strcmp(s + 1, "trusted"))
                    section = TRUSTED;
                else
                    die("%s:%d: unknown section [%s]", path, lineno, s + 1);
            } else if (section == OPTIONS) {
                char *eq = strchr(s, '=');
                if (!eq)
                    die("%s:%d: expected 'key = value'", path, lineno);
                *eq = '\0';
                char *key = strip(s);
                char *value = strip(eq + 1);
                if (!strcmp(key, "Architecture"))
                    snprintf(arch, sizeof(arch), "%s", value);
                else if (!strcmp(key, "SigLevel"))
                    siglevel = parse_siglevel(value);
                else if (!strcmp(key, "API"))
                    snprintf(api_base, sizeof(api_base), "%s", value);
                else if (!strcmp(key, "Prerelease"))
                    allow_prerelease = yes(value);
                else if (!strcmp(key, "Registry"))
                    snprintf(registry, sizeof(registry), "%s", value);
                else if (!strcmp(key, "AllowLocalRepos"))
                    allow_local = yes(value);
            } else if (section == TRUSTED) {
                if (local_count < MAX_SOURCES) {
                    snprintf(locals[local_count], PKG_PATH_MAX, "%s", value_of_line(s));
                    local_lines[local_count++] = lineno;
                }
            } else {
                die("%s:%d: setting outside of a section", path, lineno);
            }
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    free(text);
    size_t n = strlen(api_base);
    while (n > 1 && api_base[n - 1] == '/')
        api_base[--n] = '\0';
    char cache[PKG_PATH_MAX + 32];
    snprintf(cache, sizeof(cache), "%s/" REGISTRY_FILE, rp("var/lib/pkg/sync"));
    if (registry[0])
        load_registry_file(cache);
    locals_ignored = 0;
    for (int i = 0; i < local_count; i++) {
        if (allow_local)
            add_source(locals[i], 1, path, local_lines[i]);
        else
            locals_ignored++;
    }
    return 0;
}

void config_load(void)
{
    sources_total = 0;
    config_used[0] = '\0';
    registry[0] = '\0';
    allow_local = 0;
    if (opt.config) {
        load_file(opt.config, 1);
        return;
    }
    if (load_file(rp("etc/pkg/pkg.conf"), 0) == 0)
        return;
    load_file(rp("etc/pkg.conf"), 0);
}

#define CURL_QUIET 1
#define CURL_SILENT 2

static int curl_run(const char *url, const char *dest, int api, int mode, const char *headers, const char *etag,
                    int follow)
{
    char *argv[32];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = follow ? "-fL" : "-f";
    argv[n++] = "--retry";
    argv[n++] = "2";
    argv[n++] = "--connect-timeout";
    argv[n++] = "20";
    argv[n++] = "--speed-limit";
    argv[n++] = "1024";
    argv[n++] = "--speed-time";
    argv[n++] = "60";
    argv[n++] = mode == CURL_SILENT ? "-s" : (!mode && isatty(1)) ? "-#" : "-sS";
    if (api) {
        argv[n++] = "-H";
        argv[n++] = "Accept: application/vnd.github+json";
        argv[n++] = "-H";
        argv[n++] = "X-GitHub-Api-Version: 2022-11-28";
    }
    if (headers) {
        argv[n++] = "-D";
        argv[n++] = (char *)headers;
    }
    if (etag && path_exists(etag)) {
        argv[n++] = "--etag-compare";
        argv[n++] = (char *)etag;
    }
    argv[n++] = "-o";
    argv[n++] = (char *)dest;
    argv[n++] = (char *)url;
    argv[n] = NULL;
    return run_argv(argv);
}

static int curl_get(const char *url, const char *dest, int api, int mode)
{
    if (!strncmp(url, "file://", 7) || url[0] == '/')
        return copy_file(url[0] == '/' ? url : url + 7, dest);
    char tmp[PKG_PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.part", dest);
    unlink(tmp);
    if (curl_run(url, tmp, api, mode, NULL, NULL, 1) != 0 || rename(tmp, dest) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int header_status(const char *headers)
{
    char *text = read_text_file(headers, NULL);
    if (!text)
        return 0;
    int status = 0;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (!strncmp(line, "HTTP/", 5)) {
            char *sp = strchr(line, ' ');
            if (sp)
                status = atoi(sp + 1);
        }
        line = next ? next + 1 : NULL;
    }
    free(text);
    return status;
}

static int header_value(const char *headers, const char *key, char *out, size_t cap)
{
    char *text = read_text_file(headers, NULL);
    if (!text)
        return -1;
    int found = -1;
    size_t klen = strlen(key);
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next = '\0';
        if (!strncasecmp(line, key, klen) && line[klen] == ':') {
            char *v = line + klen + 1;
            while (*v == ' ')
                v++;
            size_t len = strcspn(v, "\r");
            if (len + 1 <= cap) {
                memcpy(out, v, len);
                out[len] = '\0';
                found = 0;
            }
        }
        line = next ? next + 1 : NULL;
    }
    free(text);
    return found;
}

static void cache_name(const struct source *src, const char *ext, char *out, size_t cap)
{
    char flat[96];
    snprintf(flat, sizeof(flat), "%s", src->owner_repo);
    for (char *c = flat; *c; c++)
        if (*c == '/')
            *c = '_';
    snprintf(out, cap, "%s/%s.%s", rp("var/lib/pkg/sync/cache"), flat, ext);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static int registry_where(char *base, size_t bcap, char *repo, size_t rcap)
{
    if (!registry[0])
        return -1;
    char owner_repo[96];
    if (!strncmp(registry, "file://", 7) || registry[0] == '/' || ends_with(registry, ".conf")) {
        snprintf(base, bcap, "%s", registry);
        char *slash = strrchr(base, '/');
        if (!slash)
            return -1;
        slash[1] = '\0';
        snprintf(repo, rcap, "registry");
        return 0;
    }
    if (parse_github(registry, owner_repo, sizeof(owner_repo)) < 0)
        return -1;
    snprintf(base, bcap, "https://raw.githubusercontent.com/%s/main/", owner_repo);
    snprintf(repo, rcap, "%s", owner_repo);
    return 0;
}

static int parse_asset(const char *asset, char *name, size_t ncap, char *version, size_t vcap)
{
    char suffix[40];
    const char *arches[2] = {arch, "any"};
    size_t len = 0;
    for (int i = 0; i < 2 && !len; i++) {
        snprintf(suffix, sizeof(suffix), "-%s.pkg.tar.gz", arches[i]);
        if (ends_with(asset, suffix))
            len = strlen(asset) - strlen(suffix);
    }
    if (!len)
        return -1;
    char base[PKG_FILENAME_MAX];
    if (len + 1 > sizeof(base))
        return -1;
    memcpy(base, asset, len);
    base[len] = '\0';
    char *rel = strrchr(base, '-');
    if (!rel || rel == base || !rel[1])
        return -1;
    *rel = '\0';
    char *ver = strrchr(base, '-');
    if (!ver || ver == base || !isdigit((unsigned char)ver[1]))
        return -1;
    *ver = '\0';
    if (!valid_name(base) || strlen(base) + 1 > ncap)
        return -1;
    snprintf(name, ncap, "%s", base);
    if ((size_t)snprintf(version, vcap, "%s-%s", ver + 1, rel + 1) >= vcap)
        return -1;
    return 0;
}

struct found {
    char name[PKG_NAME_MAX];
    char version[PKG_VERSION_MAX];
    char tag[64];
    char asset[PKG_FILENAME_MAX];
    char url[PKG_PATH_MAX];
    char sig[PKG_PATH_MAX];
    char desc[PKG_DESC_MAX];
    uint64_t size;
};

static void add_found(struct found *table, int *count, const char *aname, const char *tag, const char *desc,
                      const char *url, const char *sig, uint64_t size)
{
    char name[PKG_NAME_MAX], version[PKG_VERSION_MAX];
    if (parse_asset(aname, name, sizeof(name), version, sizeof(version)) < 0)
        return;
    int slot = -1;
    for (int i = 0; i < *count; i++)
        if (!strcmp(table[i].name, name))
            slot = i;
    if (slot >= 0 && pkg_vercmp(version, table[slot].version) <= 0)
        return;
    if (slot < 0) {
        if (*count == MAX_PER_REPO)
            return;
        slot = (*count)++;
    }
    struct found *f = &table[slot];
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(f->version, sizeof(f->version), "%s", version);
    snprintf(f->tag, sizeof(f->tag), "%s", tag);
    snprintf(f->asset, sizeof(f->asset), "%s", aname);
    snprintf(f->desc, sizeof(f->desc), "%s", desc);
    snprintf(f->url, sizeof(f->url), "%s", url);
    snprintf(f->sig, sizeof(f->sig), "%s", sig);
    f->size = size;
}

static void scan_release(const char *release, struct found *table, int *count)
{
    const char *v;
    if ((v = json_get(release, "draft")) && !strncmp(v, "true", 4))
        return;
    if (!allow_prerelease && (v = json_get(release, "prerelease")) && !strncmp(v, "true", 4))
        return;
    const char *assets = json_get(release, "assets");
    if (!assets || *assets != '[')
        return;
    char tag[64] = "", relname[PKG_DESC_MAX] = "";
    if ((v = json_get(release, "tag_name")))
        json_string(v, tag, sizeof(tag));
    if ((v = json_get(release, "name")))
        json_string(v, relname, sizeof(relname));
    const char *cursor = assets, *asset;
    while (json_array_next(&cursor, &asset) == 1) {
        char aname[PKG_FILENAME_MAX], url[PKG_PATH_MAX] = "", sig[PKG_PATH_MAX] = "";
        const char *nv = json_get(asset, "name");
        if (!nv || json_string(nv, aname, sizeof(aname)) < 0)
            continue;
        const char *uv = json_get(asset, "browser_download_url");
        if (uv)
            json_string(uv, url, sizeof(url));
        const char *sv = json_get(asset, "size");
        char signame[PKG_FILENAME_MAX + 8];
        snprintf(signame, sizeof(signame), "%s.sig", aname);
        const char *c2 = assets, *a2;
        while (json_array_next(&c2, &a2) == 1) {
            char n2[PKG_FILENAME_MAX + 8];
            const char *nv2 = json_get(a2, "name");
            if (!nv2 || json_string(nv2, n2, sizeof(n2)) < 0 || strcmp(n2, signame) != 0)
                continue;
            const char *u2 = json_get(a2, "browser_download_url");
            if (u2)
                json_string(u2, sig, sizeof(sig));
        }
        add_found(table, count, aname, tag, relname, url, sig, sv ? (uint64_t)json_number(sv) : 0);
    }
}

static int parse_releases(const char *path, struct found *table, int *count, char *why, size_t why_cap)
{
    char *text = read_text_file(path, NULL);
    if (!text) {
        snprintf(why, why_cap, "empty answer");
        return -1;
    }
    const char *p = json_ws(text);
    int releases = 0;
    if (p && *p == '[') {
        const char *cursor = p, *release;
        while (json_array_next(&cursor, &release) == 1) {
            releases++;
            scan_release(release, table, count);
        }
    } else {
        char message[200] = "unexpected answer";
        const char *m = p ? json_get(p, "message") : NULL;
        if (m)
            json_string(m, message, sizeof(message));
        snprintf(why, why_cap, "GitHub: %s", message);
        free(text);
        return -1;
    }
    free(text);
    return releases;
}

static int query_html(const struct source *src, struct found *table, int *count, char *why, size_t why_cap)
{
    char headers[PKG_PATH_MAX + 32], page[PKG_PATH_MAX + 32], url[PKG_PATH_MAX + 128], location[PKG_PATH_MAX];
    snprintf(headers, sizeof(headers), "%s/latest.hdr", rp("var/lib/pkg/sync/cache"));
    snprintf(page, sizeof(page), "%s/assets.html", rp("var/lib/pkg/sync/cache"));
    snprintf(url, sizeof(url), "https://github.com/%s/releases/latest", src->owner_repo);
    unlink(headers);
    curl_run(url, "/dev/null", 0, CURL_SILENT, headers, NULL, 0);
    int status = header_status(headers);
    int have = header_value(headers, "location", location, sizeof(location)) == 0;
    unlink(headers);
    if (!status) {
        snprintf(why, why_cap, "cannot reach %s", src->url);
        return -1;
    }
    const char *mark = have ? strstr(location, "/releases/tag/") : NULL;
    if (!mark) {
        snprintf(why, why_cap, status == 404 ? "repository not found" : "no releases");
        return status == 404 ? -1 : 0;
    }
    char tag[64];
    snprintf(tag, sizeof(tag), "%s", mark + strlen("/releases/tag/"));
    snprintf(url, sizeof(url), "https://github.com/%s/releases/expanded_assets/%s", src->owner_repo, tag);
    if (curl_get(url, page, 0, CURL_QUIET) < 0) {
        snprintf(why, why_cap, "cannot read the release %s", tag);
        return -1;
    }
    char *html = read_text_file(page, NULL);
    unlink(page);
    if (!html) {
        snprintf(why, why_cap, "empty answer");
        return -1;
    }
    char prefix[PKG_PATH_MAX];
    snprintf(prefix, sizeof(prefix), "/%s/releases/download/%s/", src->owner_repo, tag);
    size_t plen = strlen(prefix);
    for (const char *c = strstr(html, prefix); c; c = strstr(c + 1, prefix)) {
        const char *aname = c + plen;
        size_t len = strcspn(aname, "\"'<> ");
        if (!len || len >= PKG_FILENAME_MAX)
            continue;
        char asset[PKG_FILENAME_MAX], sigpath[PKG_PATH_MAX + 8], full[PKG_PATH_MAX + 32], sig[PKG_PATH_MAX + 40] = "";
        memcpy(asset, aname, len);
        asset[len] = '\0';
        if (!ends_with(asset, ".pkg.tar.gz"))
            continue;
        snprintf(sigpath, sizeof(sigpath), "%s%s.sig\"", prefix, asset);
        snprintf(full, sizeof(full), "https://github.com%s%s", prefix, asset);
        if (strstr(html, sigpath))
            snprintf(sig, sizeof(sig), "%s.sig", full);
        add_found(table, count, asset, tag, "", full, sig, 0);
    }
    free(html);
    return 1;
}

static int query_source(const struct source *src, struct found *table, int *count, char *why, size_t why_cap)
{
    *count = 0;
    char url[PKG_PATH_MAX + 128];
    snprintf(url, sizeof(url), "%s/repos/%s/releases?per_page=30", api_base, src->owner_repo);
    int releases;
    if (!strncmp(api_base, "file://", 7) || api_base[0] == '/') {
        char dest[PKG_PATH_MAX];
        snprintf(dest, sizeof(dest), "%s/releases.json", rp("var/lib/pkg/sync"));
        if (curl_get(url, dest, 1, CURL_QUIET) < 0) {
            snprintf(why, why_cap, "cannot reach %s", src->url);
            return -1;
        }
        releases = parse_releases(dest, table, count, why, why_cap);
        unlink(dest);
    } else {
        mkdirs(rp("var/lib/pkg/sync/cache"), 0755);
        char json[PKG_PATH_MAX + 128], etag[PKG_PATH_MAX + 128], part[PKG_PATH_MAX + 140], headers[PKG_PATH_MAX + 140];
        cache_name(src, "json", json, sizeof(json));
        cache_name(src, "etag", etag, sizeof(etag));
        snprintf(part, sizeof(part), "%s.part", json);
        snprintf(headers, sizeof(headers), "%s.hdr", json);
        unlink(part);
        unlink(headers);
        if (!path_exists(json))
            unlink(etag);
        curl_run(url, part, 1, CURL_SILENT, headers, etag, 1);
        int status = header_status(headers);
        char value[200];
        if (status == 200 && path_exists(part)) {
            rename(part, json);
            if (header_value(headers, "etag", value, sizeof(value)) == 0) {
                strcat(value, "\n");
                write_text_file(etag, value, strlen(value));
            } else {
                unlink(etag);
            }
        }
        int limited = (status == 403 || status == 429) &&
                      header_value(headers, "x-ratelimit-remaining", value, sizeof(value)) == 0 && atoi(value) == 0;
        unlink(part);
        unlink(headers);
        if (status == 200 || status == 304) {
            releases = parse_releases(json, table, count, why, why_cap);
        } else {
            int rc = query_html(src, table, count, why, why_cap);
            if (rc < 0 && limited)
                snprintf(why, why_cap, "GitHub API limit reached and %s is unreachable", "github.com");
            if (rc <= 0)
                return rc < 0 ? -1 : 1;
            releases = 1;
        }
    }
    if (releases < 0)
        return -1;
    if (!*count) {
        if (!releases)
            snprintf(why, why_cap, "no releases");
        else
            snprintf(why, why_cap, "no NAME-VERSION-REL-%s.pkg.tar.gz in the releases", arch);
        return 1;
    }
    return 0;
}

static void src_path(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.src", rp("var/lib/pkg/sync"), name);
}

static void write_src(const struct source *src, const struct found *f)
{
    char path[PKG_PATH_MAX + 80], body[4096];
    src_path(f->name, path, sizeof(path));
    int n = snprintf(body, sizeof(body),
                     "name=%s\nversion=%s\nrepo=%s\norigin=%s\ntag=%s\nasset=%s\nurl=%s\nsig=%s\nsize=%llu\n"
                     "desc=%s\n",
                     f->name, f->version, src->owner_repo, src->local ? "local" : "registry", f->tag, f->asset,
                     f->url, f->sig, (unsigned long long)f->size, f->desc);
    if (n > 0 && (size_t)n < sizeof(body))
        write_text_file(path, body, (size_t)n);
}

static int src_field(const char *path, const char *key, char *out, size_t cap)
{
    char *text = read_text_file(path, NULL);
    if (!text)
        return -1;
    size_t kl = strlen(key);
    int found = -1;
    for (char *line = text; line && *line;) {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            snprintf(out, cap, "%s", line + kl + 1);
            found = 0;
            break;
        }
        line = eol ? eol + 1 : NULL;
    }
    free(text);
    return found;
}

static void fresh_url(const char *url, int signature, char *out, size_t cap)
{
    if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))
        snprintf(out, cap, "%s%s?nocache=%lld", url, signature ? ".sig" : "", (long long)time(NULL));
    else
        snprintf(out, cap, "%s%s", url, signature ? ".sig" : "");
}

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

static int verify_embedded(const char *path)
{
    size_t length = 0;
    char *text = read_text_file(path, &length);
    if (!text)
        return 1;
    size_t end = length;
    while (end && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' '))
        end--;
    size_t start = end;
    while (start && text[start - 1] != '\n')
        start--;
    if (end - start < 6 || strncmp(text + start, "#sig ", 5) != 0) {
        free(text);
        return 1;
    }
    size_t digits = end - start - 5;
    unsigned char *der = malloc(digits / 2 + 1);
    size_t der_len = 0;
    int rc = SIG_BAD;
    if (der && digits % 2 == 0) {
        int valid = 1;
        for (size_t i = 0; i < digits; i += 2) {
            int hi = hex_value(text[start + 5 + i]), lo = hex_value(text[start + 6 + i]);
            if (hi < 0 || lo < 0) {
                valid = 0;
                break;
            }
            der[der_len++] = (unsigned char)(hi * 16 + lo);
        }
        if (valid) {
            char payload[PKG_PATH_MAX + 16], esig[PKG_PATH_MAX + 16];
            snprintf(payload, sizeof(payload), "%s.payload", path);
            snprintf(esig, sizeof(esig), "%s.esig", path);
            if (write_text_file(payload, text, start) == 0 && write_text_file(esig, der, der_len) == 0)
                rc = sig_verify_file(payload, esig);
            unlink(payload);
            unlink(esig);
        }
    }
    free(der);
    free(text);
    return rc;
}

static int download_signed(const char *url, const char *dest, const char *label, int optional)
{
    char fresh[PKG_PATH_MAX + 40], sig[PKG_PATH_MAX + 48], sigurl[PKG_PATH_MAX + 96], geturl[PKG_PATH_MAX + 96];
    snprintf(fresh, sizeof(fresh), "%s.new", dest);
    snprintf(sig, sizeof(sig), "%s.sig", fresh);
    fresh_url(url, 1, sigurl, sizeof(sigurl));
    fresh_url(url, 0, geturl, sizeof(geturl));
    if (curl_get(geturl, fresh, 0, optional ? CURL_SILENT : 1) < 0) {
        unlink(fresh);
        if (optional) {
            printf("not available\n");
            return 1;
        }
        printf("failed\n");
        warn("cannot download the %s %s, using the last copy", label, url);
        return -1;
    }
    if (siglevel != SIG_NEVER) {
        int embedded = verify_embedded(fresh);
        int have_sig = 0, rc = embedded;
        if (embedded == 1) {
            have_sig = curl_get(sigurl, sig, 0, CURL_SILENT) == 0;
            if (have_sig) {
                rc = sig_verify_file(fresh, sig);
                unlink(sig);
            }
        } else {
            have_sig = 1;
        }
        if (have_sig) {
            if (rc == SIG_BAD || (rc == SIG_NOKEYS && siglevel == SIG_REQUIRED)) {
                printf("failed\n");
                warn("the %s signature is %s, keeping the last copy", label,
                     rc == SIG_BAD ? "invalid" : "not checkable (no keys)");
                unlink(fresh);
                return -1;
            }
        } else if (siglevel == SIG_REQUIRED) {
            printf("failed\n");
            warn("the %s has no signature, SigLevel = Required", label);
            unlink(fresh);
            return -1;
        }
    }
    rename(fresh, dest);
    return 0;
}

static int fetch_registry(void)
{
    if (!registry[0])
        return 0;
    char url[PKG_PATH_MAX + 64], owner_repo[96];
    if (!strncmp(registry, "file://", 7) || registry[0] == '/' || ends_with(registry, ".conf"))
        snprintf(url, sizeof(url), "%s", registry);
    else if (parse_github(registry, owner_repo, sizeof(owner_repo)) == 0)
        snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/main/" REGISTRY_FILE, owner_repo);
    else
        die("Registry = %s is neither a GitHub repository nor a .conf address", registry);
    char dest[PKG_PATH_MAX + 32];
    snprintf(dest, sizeof(dest), "%s/" REGISTRY_FILE, rp("var/lib/pkg/sync"));
    printf(" %-36s ", "registry");
    fflush(stdout);
    if (download_signed(url, dest, "registry", 0) < 0)
        return -1;
    printf("done\n");
    return 0;
}

static char sync_written[MAX_SOURCES * 4][PKG_NAME_MAX];
static int sync_written_count;

static int sync_repository(int *fresh)
{
    *fresh = 0;
    char base[PKG_PATH_MAX], repo[96];
    if (registry_where(base, sizeof(base), repo, sizeof(repo)) < 0)
        return 0;
    char url[PKG_PATH_MAX + 32], dest[PKG_PATH_MAX + 32];
    snprintf(url, sizeof(url), "%s" REPOSITORY_FILE, base);
    snprintf(dest, sizeof(dest), "%s/" REPOSITORY_FILE, rp("var/lib/pkg/sync"));
    printf(" %-36s ", REPOSITORY_FILE);
    fflush(stdout);
    int rc = download_signed(url, dest, REPOSITORY_FILE, 1);
    if (rc < 0)
        return -1;
    *fresh = rc == 0;
    char *text = read_text_file(dest, NULL);
    if (!text) {
        if (rc == 0)
            printf("empty\n");
        return 0;
    }
    struct source tsrc;
    memset(&tsrc, 0, sizeof(tsrc));
    snprintf(tsrc.owner_repo, sizeof(tsrc.owner_repo), "%s", repo);
    int shown = 0, lineno = 0;
    for (char *line = text; line && *line;) {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';
        lineno++;
        char *next = eol ? eol + 1 : NULL;
        char *s = strip(line);
        line = next;
        if (!*s || *s == '#')
            continue;
        char *name = s, *p = s;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
        char *path = strip(p);
        char *desc = "";
        for (p = path; *p && *p != ' ' && *p != '\t'; p++)
            ;
        if (*p) {
            *p++ = '\0';
            desc = strip(p);
        }
        const char *file = strrchr(path, '/');
        file = file ? file + 1 : path;
        char pname[PKG_NAME_MAX], pversion[PKG_VERSION_MAX];
        if (!valid_name(name) || !*path || path[0] == '/' || strstr(path, "..") || strstr(path, "://") ||
            parse_asset(file, pname, sizeof(pname), pversion, sizeof(pversion)) < 0 || strcmp(pname, name) != 0) {
            if (!shown)
                printf("\n");
            shown = 1;
            warn("%s:%d: bad entry, ignored", REPOSITORY_FILE, lineno);
            continue;
        }
        int dup = 0;
        for (int w = 0; w < sync_written_count; w++)
            if (!strcmp(sync_written[w], name))
                dup = 1;
        if (dup) {
            if (!shown)
                printf("\n");
            shown = 1;
            warn("%s:%d: package %s is listed twice, skipped", REPOSITORY_FILE, lineno, name);
            continue;
        }
        struct found f;
        memset(&f, 0, sizeof(f));
        snprintf(f.name, sizeof(f.name), "%s", pname);
        snprintf(f.version, sizeof(f.version), "%s", pversion);
        snprintf(f.asset, sizeof(f.asset), "%s", file);
        snprintf(f.desc, sizeof(f.desc), "%s", desc);
        snprintf(f.url, sizeof(f.url), "%s%s", base, path);
        snprintf(f.sig, sizeof(f.sig), "%s.sig", f.url);
        write_src(&tsrc, &f);
        if (sync_written_count < (int)(sizeof(sync_written) / sizeof(sync_written[0])))
            snprintf(sync_written[sync_written_count++], PKG_NAME_MAX, "%s", pname);
        printf("%s%s %s", shown++ ? ", " : "", pname, pversion);
    }
    if (rc == 0)
        printf("%s\n", shown ? "" : "empty");
    free(text);
    return 0;
}

int sync_refresh(void)
{
    mkdirs(rp("var/lib/pkg/sync"), 0755);
    msg("Checking the registry and its repositories...");
    int failed = fetch_registry() < 0;
    char keep_config[PKG_PATH_MAX];
    snprintf(keep_config, sizeof(keep_config), "%s", config_used);
    config_load();
    if (!sources_total && !registry[0])
        die("no repositories: set Registry in %s", keep_config[0] ? keep_config : rp("etc/pkg/pkg.conf"));
    if (locals_ignored)
        warn("%d repositories in [trusted] are ignored, set AllowLocalRepos = yes to use them", locals_ignored);
    sync_written_count = 0;
    int trusted_fresh = 0;
    if (sync_repository(&trusted_fresh) < 0)
        failed++;
    char trusted_repo[96] = "", trusted_base[PKG_PATH_MAX];
    registry_where(trusted_base, sizeof(trusted_base), trusted_repo, sizeof(trusted_repo));
    int ok_repo[MAX_SOURCES] = {0};
    for (int i = 0; i < sources_total; i++) {
        const struct source *src = &sources[i];
        struct found table[MAX_PER_REPO];
        int count = 0;
        char why[300];
        printf(" %-36s ", src->owner_repo);
        fflush(stdout);
        int rc = query_source(src, table, &count, why, sizeof(why));
        if (rc != 0) {
            printf("%s\n", why);
            if (rc < 0)
                failed++;
            else
                ok_repo[i] = 1;
            continue;
        }
        ok_repo[i] = 1;
        int shown = 0;
        for (int k = 0; k < count; k++) {
            int dup = 0;
            for (int w = 0; w < sync_written_count; w++)
                if (!strcmp(sync_written[w], table[k].name))
                    dup = 1;
            if (dup) {
                printf("%s", shown ? "" : "\n");
                warn("%s: package %s is already provided by an earlier repository, skipped", src->owner_repo,
                     table[k].name);
                continue;
            }
            write_src(src, &table[k]);
            if (sync_written_count < (int)(sizeof(sync_written) / sizeof(sync_written[0])))
                snprintf(sync_written[sync_written_count++], PKG_NAME_MAX, "%s", table[k].name);
            printf("%s%s %s", shown++ ? ", " : "", table[k].name, table[k].version);
        }
        printf("%s\n", src->local ? "  (local)" : "");
    }
    DIR *d = opendir(rp("var/lib/pkg/sync"));
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (n <= 4 || strcmp(e->d_name + n - 4, ".src") != 0)
                continue;
            char name[PKG_NAME_MAX], path[PKG_PATH_MAX + 300], repo[96] = "";
            snprintf(name, sizeof(name), "%.*s", (int)(n - 4), e->d_name);
            snprintf(path, sizeof(path), "%s/%s", rp("var/lib/pkg/sync"), e->d_name);
            int keep = 0;
            for (int w = 0; w < sync_written_count; w++)
                if (!strcmp(sync_written[w], name))
                    keep = 1;
            if (!keep && src_field(path, "repo", repo, sizeof(repo)) == 0) {
                if (!trusted_fresh && trusted_repo[0] && !strcasecmp(repo, trusted_repo))
                    keep = 1;
                for (int i = 0; i < sources_total; i++)
                    if (!strcasecmp(sources[i].owner_repo, repo) && !ok_repo[i])
                        keep = 1;
            }
            if (!keep)
                unlink(path);
        }
        closedir(d);
    }
    return failed ? -1 : 0;
}

static void sync_add(const struct pkg_info *info, const struct sync_meta *meta)
{
    if (sdb.count == sdb.cap) {
        int cap = sdb.cap ? sdb.cap * 2 : 64;
        sdb.pkgs = realloc(sdb.pkgs, (size_t)cap * sizeof(*sdb.pkgs));
        sdb.meta = realloc(sdb.meta, (size_t)cap * sizeof(*sdb.meta));
        if (!sdb.pkgs || !sdb.meta)
            die("out of memory");
        sdb.cap = cap;
    }
    sdb.pkgs[sdb.count] = *info;
    sdb.meta[sdb.count] = *meta;
    sdb.count++;
}

static int source_allowed(const char *owner_repo, int *local)
{
    char base[PKG_PATH_MAX], repo[96];
    if (registry_where(base, sizeof(base), repo, sizeof(repo)) == 0 && !strcasecmp(repo, owner_repo)) {
        *local = 0;
        return 1;
    }
    for (int i = 0; i < sources_total; i++)
        if (!strcasecmp(sources[i].owner_repo, owner_repo)) {
            *local = sources[i].local;
            return 1;
        }
    return 0;
}

void syncdb_load(void)
{
    free(sdb.pkgs);
    free(sdb.meta);
    memset(&sdb, 0, sizeof(sdb));
    DIR *d = opendir(rp("var/lib/pkg/sync"));
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= 4 || strcmp(e->d_name + len - 4, ".src") != 0)
            continue;
        char path[PKG_PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", rp("var/lib/pkg/sync"), e->d_name);
        char *text = read_text_file(path, NULL);
        if (!text)
            continue;
        struct pkg_info info;
        struct sync_meta meta;
        memset(&info, 0, sizeof(info));
        memset(&meta, 0, sizeof(meta));
        for (char *line = text; line && *line;) {
            char *eol = strchr(line, '\n');
            if (eol)
                *eol = '\0';
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                const char *k = line, *v = eq + 1;
                if (!strcmp(k, "name"))
                    snprintf(info.name, sizeof(info.name), "%s", v);
                else if (!strcmp(k, "version"))
                    snprintf(info.version, sizeof(info.version), "%s", v);
                else if (!strcmp(k, "repo"))
                    snprintf(meta.repo, sizeof(meta.repo), "%s", v);
                else if (!strcmp(k, "tag"))
                    snprintf(meta.tag, sizeof(meta.tag), "%s", v);
                else if (!strcmp(k, "asset"))
                    snprintf(info.filename, sizeof(info.filename), "%s", v);
                else if (!strcmp(k, "url"))
                    snprintf(meta.url, sizeof(meta.url), "%s", v);
                else if (!strcmp(k, "sig"))
                    snprintf(meta.sig, sizeof(meta.sig), "%s", v);
                else if (!strcmp(k, "size"))
                    info.csize = strtoull(v, NULL, 10);
                else if (!strcmp(k, "desc"))
                    snprintf(info.desc, sizeof(info.desc), "%s", v);
            }
            line = eol ? eol + 1 : NULL;
        }
        free(text);
        int local = 0;
        if (!info.name[0] || !info.version[0] || !meta.url[0] || !info.filename[0] ||
            !source_allowed(meta.repo, &local))
            continue;
        meta.local = local;
        snprintf(info.arch, sizeof(info.arch), "%s", arch);
        sync_add(&info, &meta);
    }
    closedir(d);
}

int syncdb_find(const char *name)
{
    for (int i = 0; i < sdb.count; i++)
        if (!strcmp(sdb.pkgs[i].name, name))
            return i;
    return -1;
}

const char *package_for_url(const char *url, char *why, size_t cap)
{
    char owner_repo[96];
    if (parse_github(url, owner_repo, sizeof(owner_repo)) < 0) {
        snprintf(why, cap, "%s is not a GitHub repository URL", url);
        return NULL;
    }
    int local = 0;
    if (!source_allowed(owner_repo, &local)) {
        snprintf(why, cap, "%s is not in the registry%s", owner_repo, allow_local ? " or in [trusted]" : "");
        return NULL;
    }
    const char *found = NULL;
    int count = 0;
    for (int i = 0; i < sdb.count; i++)
        if (!strcasecmp(sdb.meta[i].repo, owner_repo)) {
            found = sdb.pkgs[i].name;
            count++;
        }
    if (count == 1)
        return found;
    if (!count)
        snprintf(why, cap, "%s has no packages (run 'pkg -Sy')", owner_repo);
    else
        snprintf(why, cap, "%s provides %d packages, name one of them (pkg -Sl)", owner_repo, count);
    return NULL;
}

static int check_signature(const char *file, const struct sync_meta *m, int quiet)
{
    if (siglevel == SIG_NEVER)
        return 0;
    if (!m->sig[0]) {
        if (siglevel == SIG_REQUIRED) {
            if (!quiet)
                warn("%s: the release has no signature, SigLevel = Required", file);
            return -1;
        }
        return 0;
    }
    char sigfile[PKG_PATH_MAX + 8];
    snprintf(sigfile, sizeof(sigfile), "%s.sig", file);
    if (!path_exists(sigfile) && curl_get(m->sig, sigfile, 0, 1) < 0) {
        if (!quiet)
            warn("cannot download the signature %s", m->sig);
        return siglevel == SIG_REQUIRED ? -1 : 0;
    }
    int rc = sig_verify_file(file, sigfile);
    if (rc == SIG_OK)
        return 0;
    if (rc == SIG_NOKEYS && siglevel != SIG_REQUIRED) {
        if (!quiet)
            warn("signature not checked, no keys in %s", rp("etc/pkg/keys"));
        return 0;
    }
    if (!quiet)
        warn("%s: %s", file, rc == SIG_BAD ? "signature is invalid" : "no trusted key to check the signature");
    unlink(sigfile);
    return -1;
}

int fetch_package(int index, char *out_path, size_t cap)
{
    const struct pkg_info *p = &sdb.pkgs[index];
    const struct sync_meta *m = &sdb.meta[index];
    if (!p->filename[0] || strchr(p->filename, '/'))
        return -1;
    mkdirs(rp("var/cache/pkg"), 0755);
    snprintf(out_path, cap, "%s/%s", rp("var/cache/pkg"), p->filename);
    struct stat st;
    if (stat(out_path, &st) == 0 && (!p->csize || (uint64_t)st.st_size == p->csize) &&
        check_signature(out_path, m, 1) == 0)
        return 0;
    unlink(out_path);
    char sigfile[PKG_PATH_MAX + 8];
    snprintf(sigfile, sizeof(sigfile), "%s.sig", out_path);
    unlink(sigfile);
    printf(" %s (%s%s)\n", p->filename, m->repo, m->local ? ", local" : "");
    if (curl_get(m->url, out_path, 0, 0) < 0) {
        warn("download failed: %s", m->url);
        return -1;
    }
    if (p->csize && stat(out_path, &st) == 0 && (uint64_t)st.st_size != p->csize) {
        warn("%s: size does not match the release", p->filename);
        unlink(out_path);
        return -1;
    }
    if (check_signature(out_path, m, 0) < 0) {
        unlink(out_path);
        return -1;
    }
    return 0;
}

void list_sources(void)
{
    printf("Registry: %s\n", registry[0] ? registry : "(none)");
    char tbase[PKG_PATH_MAX], trepo[96];
    if (registry_where(tbase, sizeof(tbase), trepo, sizeof(trepo)) == 0) {
        printf(REPOSITORY_FILE "\n");
        for (int k = 0; k < sdb.count; k++)
            if (!strcasecmp(sdb.meta[k].repo, trepo))
                printf("    %-24s %s\n", sdb.pkgs[k].name, sdb.pkgs[k].version);
    }
    for (int i = 0; i < sources_total; i++) {
        printf("%s%s\n", sources[i].url, sources[i].local ? "  (local)" : "");
        for (int k = 0; k < sdb.count; k++)
            if (!strcasecmp(sdb.meta[k].repo, sources[i].owner_repo))
                printf("    %-24s %s\n", sdb.pkgs[k].name, sdb.pkgs[k].version);
    }
    if (locals_ignored)
        printf("(%d repositories in [trusted] ignored: AllowLocalRepos is off)\n", locals_ignored);
}
