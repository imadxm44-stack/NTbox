/*
 * scm.c — مُسيّر خدمات NT-Box مبنيٌّ على خلية SYSTEM الحقيقية (Win7).
 *
 * قراءة-only من الريجستري: لا نكتب ولا نخترع بيانات.
 * كل الأرقام المطبعة في هذا الملف **مقيسة** من
 *   <win7root>/Windows/System32/config/SYSTEM
 *
 * المصابيح (NTBOX.md §9):
 *   SVC-001 OK  — جدول الخدمات مبنيٌّ من الريجستري + دورة حياة الحالة
 *   SVC-004 OK  — نواة SCM تعمل في وضع المستخدم (بديل services.exe)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "scm.h"

/* ------------------------------------------------------------------ */
/* البنية الداخلية                                                    */
/* ------------------------------------------------------------------ */
struct ntb0_scm {
    ntb0_hive *hive;
    char       hive_path[1024];
    char       control_set[32];
    uint32_t   services_key;
    int        count;
    ntb0_svc  *svcs;

    int       *order;        /* موضع i ⇒ فهرس الخدمة في الجدول        */
    int        order_count;
    int       *cycle_nodes;  /* كل الخدمات المشتركة في دورات           */
    int        cycle_node_count;
    int       *cycle_path;   /* مسار أول دورة (يتكرّر آخر عنصر)        */
    int        cycle_path_len;

    unsigned   seq;          /* عدّاد تسلسل حركات الحالة               */
};

/* ------------------------------------------------------------------ */
/* أدوات صغيرة                                                       */
/* ------------------------------------------------------------------ */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* يحوّل UTF-16LE ⇒ UTF-8 (محاولة واحدة، تتوقّف عند NUL) */
static size_t u16_to_u8(const uint8_t *data, uint32_t size, char *out, size_t len)
{
    uint32_t i;
    size_t o = 0;

    if (!out || len == 0)
        return 0;
    out[0] = '\0';
    if (!data)
        return 0;

    for (i = 0; i + 1 < size && o + 1 < len; i += 2) {
        uint16_t c = rd16(data + i);

        if (c == 0)
            break;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= len)
                break;
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= len)
                break;
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }

    out[o] = '\0';
    return o;
}

/* يبحث عن قيمة باسم معيّن (بدون مصابيح REG-005/006 المزعجة) */
static int read_value(ntb0_hive *h, uint32_t key, const char *name,
                      uint32_t *type, const uint8_t **data, uint32_t *size)
{
    int n = ntb0_hive_key_value_count(h, key);
    int i;

    for (i = 0; i < n; i++) {
        char nm[256];

        if (ntb0_hive_key_value(h, key, i, nm, sizeof(nm), type, data, size) == 0 &&
            strcasecmp(nm, name) == 0)
            return 1;
    }
    return 0;
}

/* يقرأ قيمة نصّية (SZ/EXPAND_SZ) */
static void read_str(ntb0_hive *h, uint32_t key, const char *name, char *out,
                      size_t len)
{
    uint32_t type = 0, size = 0;
    const uint8_t *data = NULL;

    if (out && len)
        out[0] = '\0';
    if (!read_value(h, key, name, &type, &data, &size))
        return;
    ntb0_hive_value_to_utf8(type, data, size, out, len);
}

/* يقرأ قيمة DWORD */
static uint32_t read_dword(ntb0_hive *h, uint32_t key, const char *name,
                            uint32_t def)
{
    uint32_t type = 0, size = 0;
    const uint8_t *data = NULL;

    if (!read_value(h, key, name, &type, &data, &size))
        return def;
    if (type == NTB0_REG_DWORD && data && size >= 4)
        return rd32(data);
    if (type == NTB0_REG_SZ && data && size >= 4)
        return rd32(data);
    return def;
}

/* يقسّم REG_MULTI_SZ إلى أسطر (الفاصل في ويندوز هو NUL)
 * out: مصفوفة أسطر، stride: حجم السطر الواحد */
static int split_multisz(const uint8_t *data, uint32_t size, char *out, int max,
                         size_t stride)
{
    uint32_t i = 0;
    int n = 0;

    if (!data || !out || max <= 0 || stride == 0)
        return 0;

    while (i + 1 < size && n < max) {
        uint32_t start = i;
        size_t o = 0;
        char *dst = out + (size_t)n * stride;

        while (i + 1 < size) {
            uint16_t c = rd16(data + i);

            if (c == 0) {
                i += 2;
                break;
            }
            if (c < 0x80) {
                if (o + 1 < stride)
                    dst[o++] = (char)c;
            } else if (c < 0x800) {
                if (o + 2 < stride) {
                    dst[o++] = (char)(0xC0 | (c >> 6));
                    dst[o++] = (char)(0x80 | (c & 0x3F));
                }
            } else {
                if (o + 3 < stride) {
                    dst[o++] = (char)(0xE0 | (c >> 12));
                    dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                    dst[o++] = (char)(0x80 | (c & 0x3F));
                }
            }
            i += 2;
        }

        if (i == start)
            break;              /* سلسلة فارغة ⇒ نهاية القائمة */
        dst[o] = '\0';
        if (o == 0)
            break;              /* عنصر فارغ ⇒ نهاية القائمة */
        n++;
    }

    return n;
}

/* ------------------------------------------------------------------ */
/* البحث في الجدول                                                    */
/* ------------------------------------------------------------------ */
ntb0_svc *ntb0_scm_find(const ntb0_scm *scm, const char *name)
{
    int i;

    if (!scm || !name || !*name)
        return NULL;

    for (i = 0; i < scm->count; i++) {
        if (strcasecmp(scm->svcs[i].name, name) == 0)
            return &scm->svcs[i];
    }
    return NULL;
}

/* فهرس الخدمة داخل الجدول (-1) */
static int find_index(const ntb0_scm *scm, const char *name)
{
    int i;

    if (!scm || !name || !*name)
        return -1;

    for (i = 0; i < scm->count; i++) {
        if (strcasecmp(scm->svcs[i].name, name) == 0)
            return i;
    }
    return -1;
}

int ntb0_scm_count(const ntb0_scm *scm)
{
    return scm ? scm->count : 0;
}

ntb0_svc *ntb0_scm_at(const ntb0_scm *scm, int index)
{
    if (!scm || index < 0 || index >= scm->count)
        return NULL;
    return &scm->svcs[index];
}

const char *ntb0_scm_name(const ntb0_scm *scm, int index)
{
    ntb0_svc *s = ntb0_scm_at(scm, index);

    return s ? s->name : NULL;
}

const char *ntb0_scm_hive_path(const ntb0_scm *scm)
{
    return scm ? scm->hive_path : NULL;
}

const char *ntb0_scm_control_set(const ntb0_scm *scm)
{
    return scm ? scm->control_set : NULL;
}

int ntb0_scm_deps(const ntb0_scm *scm, const char *name, const char **out, int max)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);
    int i, n;

    if (!s)
        return 0;
    n = s->ndeps;
    if (out && max > 0) {
        for (i = 0; i < n && i < max; i++)
            out[i] = s->deps[i];
    }
    return n;
}

int ntb0_scm_privs(const ntb0_scm *scm, const char *name, const char **out, int max)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);
    int i, n;

    if (!s)
        return 0;
    n = s->nprivs;
    if (out && max > 0) {
        for (i = 0; i < n && i < max; i++)
            out[i] = s->privs[i];
    }
    return n;
}

/* خدمات مستضافة داخل svchost.exe (Type == 0x20 share process) */
int ntb0_scm_hosted(const ntb0_scm *scm, const char **out, int max)
{
    int i, n = 0;

    if (!scm)
        return 0;

    for (i = 0; i < scm->count; i++) {
        ntb0_svc *s = &scm->svcs[i];

        if ((s->type & 0xFFu) == NTB0_SVC_TYPE_WIN32_SHARE) {
            if (out && n < max)
                out[n] = s->name;
            n++;
        }
    }
    return n;
}

/* المجموعات (svchost groups) المختلفة */
int ntb0_scm_groups(const ntb0_scm *scm, const char **out, int max)
{
    static char seen[256][NTB0_SCM_MAX_GROUP];
    static int  seen_n;
    int i, j, n = 0;

    if (!scm)
        return 0;

    seen_n = 0;
    for (i = 0; i < scm->count; i++) {
        ntb0_svc *s = &scm->svcs[i];
        int dup = 0;

        if (!s->group[0])
            continue;
        for (j = 0; j < seen_n; j++) {
            if (strcasecmp(seen[j], s->group) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        if (seen_n < (int)(sizeof(seen) / sizeof(seen[0])))
            snprintf(seen[seen_n++], NTB0_SCM_MAX_GROUP, "%s", s->group);
        if (out && n < max)
            out[n] = s->group;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* ترتيب البدء — فرز طوبولوجي حقيقي (Kahn) + كشف الدورات            */
/* ------------------------------------------------------------------ */
struct dfs_ctx {
    const int *dep_off;
    const int *dep_list;
    int       *color;      /* 0=أبيض 1=رمادي 2=أسود */
    int       *path;
    int        plen;
    int       *out;
    int        out_n;
    int        out_max;
};

static int dfs_cycle(struct dfs_ctx *c, int u)
{
    int e;

    c->color[u] = 1;
    c->path[c->plen++] = u;

    for (e = c->dep_off[u]; e < c->dep_off[u + 1]; e++) {
        int v = c->dep_list[e];

        if (c->color[v] == 1) {
            int k, start = 0;

            for (k = 0; k < c->plen; k++) {
                if (c->path[k] == v) {
                    start = k;
                    break;
                }
            }
            for (k = start; k < c->plen && c->out_n < c->out_max; k++)
                c->out[c->out_n++] = c->path[k];
            if (c->out_n < c->out_max)
                c->out[c->out_n++] = v;
            return 1;
        }
        if (c->color[v] == 0 && dfs_cycle(c, v))
            return 1;
    }

    c->color[u] = 2;
    c->plen--;
    return 0;
}

/* يعيد بناء ترتيب البدء — يستدعيه كل من يستدعي create_service */
static void compute_order(ntb0_scm *scm)
{
    int n = scm->count;
    int *dep_off = NULL, *dep_list = NULL, *rev_off = NULL, *rev_list = NULL;
    int *rev_cnt = NULL, *indeg = NULL, *done = NULL, *cur = NULL;
    int i, j, e, total = 0, emitted = 0;

    free(scm->order);
    free(scm->cycle_nodes);
    free(scm->cycle_path);
    scm->order = NULL;
    scm->cycle_nodes = NULL;
    scm->cycle_path = NULL;
    scm->order_count = 0;
    scm->cycle_node_count = 0;
    scm->cycle_path_len = 0;

    for (i = 0; i < n; i++)
        scm->svcs[i].order = -1;

    if (n <= 0)
        return;

    dep_off = (int *)calloc((size_t)n + 1, sizeof(int));
    dep_list = (int *)calloc((size_t)n * NTB0_SCM_MAX_DEPS + 1, sizeof(int));
    indeg = (int *)calloc((size_t)n, sizeof(int));
    done = (int *)calloc((size_t)n, sizeof(int));
    rev_cnt = (int *)calloc((size_t)n, sizeof(int));
    rev_off = (int *)calloc((size_t)n + 1, sizeof(int));
    rev_list = (int *)calloc((size_t)n * NTB0_SCM_MAX_DEPS + 1, sizeof(int));
    cur = (int *)calloc((size_t)n, sizeof(int));
    if (!dep_off || !dep_list || !indeg || !done || !rev_cnt || !rev_off ||
        !rev_list || !cur)
        goto out;

    /* 1) حلّ التبعيات ⇒ فهارس */
    for (i = 0; i < n; i++) {
        ntb0_svc *s = &scm->svcs[i];

        dep_off[i] = total;
        for (j = 0; j < s->ndeps; j++) {
            int d = find_index(scm, s->deps[j]);

            if (d < 0 || d == i)
                continue;              /* تبعية خارجية أو ذاتية */
            dep_list[total++] = d;
            indeg[i]++;
            rev_cnt[d]++;
        }
    }
    dep_off[n] = total;

    /* 2) القوائم العكسية (dep ← يعتمد عليها) */
    for (i = 0; i < n; i++) {
        rev_off[i + 1] = rev_off[i] + rev_cnt[i];
        cur[i] = rev_off[i];
    }
    for (i = 0; i < n; i++) {
        for (e = dep_off[i]; e < dep_off[i + 1]; e++)
            rev_list[cur[dep_list[e]]++] = i;
    }

    /* 3) Kahn — نختار دائماً أقل فهرس متاح ⇒ ترتيب حتمي */
    scm->order = (int *)calloc((size_t)n, sizeof(int));
    if (!scm->order)
        goto out;

    while (1) {
        int pick = -1;

        for (i = 0; i < n; i++) {
            if (!done[i] && indeg[i] == 0) {
                pick = i;
                break;
            }
        }
        if (pick < 0)
            break;

        done[pick] = 1;
        scm->order[emitted++] = pick;
        scm->svcs[pick].order = emitted - 1;

        for (e = rev_off[pick]; e < rev_off[pick + 1]; e++)
            indeg[rev_list[e]]--;      /* درجة مُنقصة ⇒ خدمة أخرى تصبح جاهزة */
    }
    scm->order_count = emitted;

    /* 4) ما تبقّى = داخل دورات ⇒ كشف المسار */
    {
        struct dfs_ctx ctx;
        int *color = (int *)calloc((size_t)n, sizeof(int));
        int *path = (int *)calloc((size_t)n + 1, sizeof(int));
        int *cyc = (int *)calloc((size_t)n + 1, sizeof(int));
        int *nodes = (int *)calloc((size_t)n, sizeof(int));
        int nnodes = 0;

        if (color && path && cyc && nodes) {
            ctx.dep_off = dep_off;
            ctx.dep_list = dep_list;
            ctx.color = color;
            ctx.path = path;
            ctx.plen = 0;
            ctx.out = cyc;
            ctx.out_n = 0;
            ctx.out_max = n + 1;

            for (i = 0; i < n; i++) {
                int k;

                if (done[i])
                    continue;

                ctx.plen = 0;
                ctx.out_n = 0;
                memset(color, 0, (size_t)n * sizeof(int));

                if (!dfs_cycle(&ctx, i))
                    continue;

                if (scm->cycle_path_len == 0 && ctx.out_n > 0) {
                    scm->cycle_path = (int *)calloc((size_t)ctx.out_n, sizeof(int));
                    if (scm->cycle_path) {
                        memcpy(scm->cycle_path, cyc,
                               (size_t)ctx.out_n * sizeof(int));
                        scm->cycle_path_len = ctx.out_n;
                    }
                }

                for (k = 0; k < ctx.out_n; k++) {
                    int v = cyc[k];
                    int m, seen = 0;

                    for (m = 0; m < nnodes; m++) {
                        if (nodes[m] == v) {
                            seen = 1;
                            break;
                        }
                    }
                    if (!seen)
                        nodes[nnodes++] = v;
                }
            }

            scm->cycle_nodes = nodes;
            scm->cycle_node_count = nnodes;
            free(color);
            free(path);
            free(cyc);
        } else {
            free(color);
            free(path);
            free(cyc);
            free(nodes);
        }
    }

out:
    free(dep_off);
    free(dep_list);
    free(rev_off);
    free(rev_list);
    free(rev_cnt);
    free(indeg);
    free(done);
    free(cur);
}

int ntb0_scm_start_order(const ntb0_scm *scm, int *out, int max)
{
    int i, n;

    if (!scm)
        return 0;
    n = scm->order_count;
    if (out && max > 0) {
        for (i = 0; i < n && i < max; i++)
            out[i] = scm->order[i];
    }
    return n;
}

int ntb0_scm_order_of(const ntb0_scm *scm, const char *name)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);

    return s ? s->order : -1;
}

int ntb0_scm_cycles(const ntb0_scm *scm, int *out, int max)
{
    int i, n;

    if (!scm)
        return 0;
    n = scm->cycle_node_count;
    if (out && max > 0) {
        for (i = 0; i < n && i < max; i++)
            out[i] = scm->cycle_nodes[i];
    }
    return n;
}

int ntb0_scm_cycle_report(const ntb0_scm *scm, char *buf, size_t len)
{
    int i;
    size_t o = 0;

    if (!buf || len == 0)
        return -1;
    buf[0] = '\0';
    if (!scm || scm->cycle_path_len <= 0)
        return 0;

    for (i = 0; i < scm->cycle_path_len; i++) {
        const char *nm = scm->svcs[scm->cycle_path[i]].name;
        size_t l = strlen(nm);

        if (o + l + 4 >= len)
            break;
        memcpy(buf + o, nm, l);
        o += l;
        buf[o++] = ' ';
        buf[o++] = '-';
        buf[o++] = '>';
        buf[o++] = ' ';
    }
    buf[o] = '\0';
    return (int)o;
}

/* تحقّق مستقل: كل تبعية معروفة يجب أن تسبق صاحبها في الترتيب */
int ntb0_scm_check_order(const ntb0_scm *scm)
{
    int i, j, bad = 0;

    if (!scm)
        return 0;

    for (i = 0; i < scm->count; i++) {
        ntb0_svc *s = &scm->svcs[i];

        if (s->order < 0)
            continue;                       /* داخل دورة ⇒ خارج الترتيب */
        for (j = 0; j < s->ndeps; j++) {
            ntb0_svc *d = ntb0_scm_find(scm, s->deps[j]);

            if (!d)
                continue;                   /* تبعية خارجية */
            if (d->order < 0)
                continue;                   /* ضمن دورة ⇒ تُعالَج منفصلة */
            if (d->order >= s->order)
                bad++;
        }
    }
    return bad == 0;
}

/* ------------------------------------------------------------------ */
/* تحميل الجدول من الريجستري                                          */
/* ------------------------------------------------------------------ */
static void load_service(ntb0_scm *scm, ntb0_svc *s, uint32_t key)
{
    uint32_t type = 0, size = 0;
    const uint8_t *data = NULL;

    read_str(scm->hive, key, "DisplayName", s->display, sizeof(s->display));
    read_str(scm->hive, key, "Description", s->description, sizeof(s->description));
    read_str(scm->hive, key, "ImagePath", s->image, sizeof(s->image));
    read_str(scm->hive, key, "ObjectName", s->object, sizeof(s->object));
    read_str(scm->hive, key, "Group", s->group, sizeof(s->group));
    read_str(scm->hive, key, "ServiceDll", s->dll, sizeof(s->dll));

    s->type = read_dword(scm->hive, key, "Type", 0);
    s->start = read_dword(scm->hive, key, "Start", NTB0_SVC_START_DEMAND);
    s->error_control = read_dword(scm->hive, key, "ErrorControl", 1);
    s->sid_type = read_dword(scm->hive, key, "ServiceSidType", 0);

    if (read_value(scm->hive, key, "DependOnService", &type, &data, &size)) {
        if (type == NTB0_REG_MULTI_SZ) {
            s->ndeps = split_multisz(data, size, &s->deps[0][0],
                                     NTB0_SCM_MAX_DEPS, NTB0_SCM_MAX_DEP_NAME);
        } else if (type == NTB0_REG_SZ || type == NTB0_REG_EXPAND_SZ) {
            char tmp[NTB0_SCM_MAX_DEP_NAME];

            u16_to_u8(data, size, tmp, sizeof(tmp));
            if (tmp[0]) {
                snprintf(s->deps[0], NTB0_SCM_MAX_DEP_NAME, "%s", tmp);
                s->ndeps = 1;
            }
        }
    }

    if (read_value(scm->hive, key, "RequiredPrivileges", &type, &data, &size)) {
        if (type == NTB0_REG_MULTI_SZ) {
            s->nprivs = split_multisz(data, size, &s->privs[0][0],
                                      NTB0_SCM_MAX_PRIVS, NTB0_SCM_MAX_PRIV_NAME);
        }
    }

    s->state = (s->start == NTB0_SVC_START_DISABLED) ? NTB0_SVC_DISABLED
                                                      : NTB0_SVC_STOPPED;
    s->order = -1;
}

/* يبني الجدول من ControlSet001\services (أو 002 / Current) */
static int load_services(ntb0_scm *scm)
{
    static const char *cs_names[] = {
        "ControlSet001", "ControlSet002", "CurrentControlSet"
    };
    uint32_t key = 0xFFFFFFFFu;
    int n, i, cnt = 0;

    for (i = 0; i < (int)(sizeof(cs_names) / sizeof(cs_names[0])); i++) {
        char path[128];

        snprintf(path, sizeof(path), "%s\\services", cs_names[i]);
        key = ntb0_hive_open_path(scm->hive, path);
        if (key != 0xFFFFFFFFu) {
            snprintf(scm->control_set, sizeof(scm->control_set), "%s",
                     cs_names[i]);
            break;
        }
        key = 0xFFFFFFFFu;
    }

    if (key == 0xFFFFFFFFu) {
        NTB0_FAIL("SVC-001", "no services key found in hive '%s'",
                  scm->hive_path);
        return -1;
    }
    scm->services_key = key;

    n = ntb0_hive_key_subkey_count(scm->hive, key);
    if (n <= 0) {
        NTB0_FAIL("SVC-001", "services key '%s' is empty", scm->control_set);
        return -1;
    }

    scm->svcs = (ntb0_svc *)calloc((size_t)n, sizeof(ntb0_svc));
    if (!scm->svcs) {
        NTB0_FAIL("SVC-001", "out of memory for %d service record(s)", n);
        return -1;
    }

    for (i = 0; i < n; i++) {
        char nm[256];
        uint32_t sub;

        if (ntb0_hive_key_subkey(scm->hive, key, i, nm, sizeof(nm)) < 0)
            continue;
        sub = ntb0_hive_key_subkey_off(scm->hive, key, i);
        if (sub == 0xFFFFFFFFu)
            continue;

        snprintf(scm->svcs[cnt].name, NTB0_SCM_MAX_NAME, "%.*s",
                 NTB0_SCM_MAX_NAME - 1, nm);
        load_service(scm, &scm->svcs[cnt], sub);
        cnt++;
    }

    scm->count = cnt;
    compute_order(scm);
    return cnt;
}

/* ------------------------------------------------------------------ */
/* الفتح والإغلاق                                                     */
/* ------------------------------------------------------------------ */
ntb0_scm *ntb0_scm_open(const char *hive_path)
{
    ntb0_scm *scm;

    if (!hive_path || !*hive_path) {
        NTB0_FAIL("SVC-001", "empty hive path");
        return NULL;
    }

    scm = (ntb0_scm *)calloc(1, sizeof(*scm));
    if (!scm) {
        NTB0_FAIL("SVC-001", "out of memory for the SCM struct");
        return NULL;
    }

    scm->hive = ntb0_hive_open(hive_path);
    if (!scm->hive) {
        NTB0_FAIL("SVC-001", "cannot open hive '%s'", hive_path);
        free(scm);
        return NULL;
    }

    snprintf(scm->hive_path, sizeof(scm->hive_path), "%s", hive_path);
    scm->services_key = 0xFFFFFFFFu;

    if (load_services(scm) <= 0) {
        ntb0_hive_close(scm->hive);
        free(scm);
        return NULL;
    }

    NTB0_OK("SVC-001", "SCM loaded from '%s' (%s): %d service(s), %d in start order, %d in dependency cycle(s)",
            hive_path, scm->control_set, scm->count, scm->order_count,
            scm->cycle_node_count);
    return scm;
}

ntb0_scm *ntb0_scm_open_default(void)
{
    char path[1024];

    if (ntb0_hive_config_path("SYSTEM", path, sizeof(path)) < 0) {
        NTB0_FAIL("SVC-001", "SYSTEM hive not found under the Win7 root");
        return NULL;
    }
    return ntb0_scm_open(path);
}

void ntb0_scm_close(ntb0_scm *scm)
{
    if (!scm)
        return;
    ntb0_hive_close(scm->hive);
    free(scm->svcs);
    free(scm->order);
    free(scm->cycle_nodes);
    free(scm->cycle_path);
    free(scm);
}

/* ------------------------------------------------------------------ */
/* دورة حياة الحالة                                                   */
/* ------------------------------------------------------------------ */
int ntb0_scm_state_of(const ntb0_scm *scm, const char *name)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);

    return s ? s->state : NTB0_SCM_ERR_NOTFOUND;
}

int ntb0_scm_start_service(ntb0_scm *scm, const char *name)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);
    int j;

    if (!s)
        return NTB0_SCM_ERR_NOTFOUND;

    if (s->start == NTB0_SVC_START_DISABLED) {
        s->state = NTB0_SVC_DISABLED;
        /*
         * رفض صحيح: خدمة معطّلة لا تُشغَّل ⇒ مصباح OK لا FAIL
         * (عقد المصابيح: الرفض المتوقَّع ليس خطأً).
         */
        NTB0_OK("SVC-001", "service '%s' is disabled (Start=4) — start refused",
                s->name);
        return NTB0_SCM_ERR_DISABLED;
    }

    if (s->state == NTB0_SVC_RUNNING)
        return NTB0_SCM_OK;

    /* التبعيات يجب أن تكون قيد التشغيل قبلها */
    for (j = 0; j < s->ndeps; j++) {
        ntb0_svc *d = ntb0_scm_find(scm, s->deps[j]);

        if (!d)
            continue;                   /* تبعية خارجية */
        if (d->state != NTB0_SVC_RUNNING) {
            /* رفض صحيح (تبعية لم تُشغَّل بعد) ⇒ مصباح OK لا FAIL */
            NTB0_OK("SVC-001",
                    "service '%s' waits for dependency '%s' (state %d) — start refused",
                    s->name, d->name, d->state);
            return NTB0_SCM_ERR_DEPENDENCY;
        }
    }

    s->state = NTB0_SVC_START_PENDING;
    s->seq_start = ++scm->seq;
    /* ملاحظة أمانة: لا نُشغّل العملية — ننتقل فقط إلى RUNNING.
     * تشغيل الملف الحقيقي (T-165) عملٌ لاحق. */
    s->state = NTB0_SVC_RUNNING;

    NTB0_OK("SVC-001", "service '%s' START_PENDING -> RUNNING (order #%d, seq %u, image '%s')",
            s->name, s->order, s->seq_start,
            s->image[0] ? s->image : "(none)");
    return NTB0_SCM_OK;
}

int ntb0_scm_stop_service(ntb0_scm *scm, const char *name)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);

    if (!s)
        return NTB0_SCM_ERR_NOTFOUND;

    if (s->state == NTB0_SVC_STOPPED || s->state == NTB0_SVC_DISABLED)
        return NTB0_SCM_OK;

    s->state = NTB0_SVC_STOP_PENDING;
    s->seq_stop = ++scm->seq;
    s->state = NTB0_SVC_STOPPED;

    NTB0_OK("SVC-001", "service '%s' STOP_PENDING -> STOPPED (seq %u)", s->name,
            s->seq_stop);
    return NTB0_SCM_OK;
}

int ntb0_scm_control_service(ntb0_scm *scm, const char *name, uint32_t control)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);

    if (!s)
        return NTB0_SCM_ERR_NOTFOUND;

    switch (control) {
    case NTB0_SVC_CONTROL_STOP:
        return ntb0_scm_stop_service(scm, name);
    case NTB0_SVC_CONTROL_PAUSE:
        if (s->state != NTB0_SVC_RUNNING)
            return NTB0_SCM_ERR_STATE;
        s->state = NTB0_SVC_PAUSED;
        NTB0_OK("SVC-001", "service '%s' paused", s->name);
        return NTB0_SCM_OK;
    case NTB0_SVC_CONTROL_CONTINUE:
        if (s->state != NTB0_SVC_PAUSED)
            return NTB0_SCM_ERR_STATE;
        s->state = NTB0_SVC_RUNNING;
        NTB0_OK("SVC-001", "service '%s' continued", s->name);
        return NTB0_SCM_OK;
    case NTB0_SVC_CONTROL_INTERROGATE:
        return NTB0_SCM_OK;      /* الحالة تُقرأ عبر ntb0_scm_state_of */
    default:
        return NTB0_SCM_ERR_RANGE;
    }
}

/* CreateService — في الذاكرة فقط (الريجستري قراءة في هذه الطبقة) */
int ntb0_scm_create_service(ntb0_scm *scm, const char *name, const char *display,
                            const char *image, uint32_t type, uint32_t start,
                            uint32_t error_control, const char *const *deps,
                            int ndeps)
{
    ntb0_svc *ns;
    int i;

    if (!scm || !name || !*name)
        return NTB0_SCM_ERR_RANGE;

    if (ntb0_scm_find(scm, name))
        return NTB0_SCM_ERR_EXISTS;

    ns = (ntb0_svc *)realloc(scm->svcs,
                             (size_t)(scm->count + 1) * sizeof(ntb0_svc));
    if (!ns)
        return NTB0_SCM_ERR_MEMORY;
    scm->svcs = ns;

    memset(&scm->svcs[scm->count], 0, sizeof(ntb0_svc));
    snprintf(scm->svcs[scm->count].name, NTB0_SCM_MAX_NAME, "%s", name);
    if (display)
        snprintf(scm->svcs[scm->count].display, NTB0_SCM_MAX_DISPLAY, "%s",
                 display);
    if (image)
        snprintf(scm->svcs[scm->count].image, NTB0_SCM_MAX_IMAGE, "%s", image);
    scm->svcs[scm->count].type = type;
    scm->svcs[scm->count].start = start;
    scm->svcs[scm->count].error_control = error_control;
    scm->svcs[scm->count].state =
        (start == NTB0_SVC_START_DISABLED) ? NTB0_SVC_DISABLED
                                           : NTB0_SVC_STOPPED;

    for (i = 0; deps && i < ndeps && i < NTB0_SCM_MAX_DEPS; i++) {
        if (!deps[i] || !*deps[i])
            continue;
        snprintf(scm->svcs[scm->count].deps[i], NTB0_SCM_MAX_DEP_NAME, "%s",
                 deps[i]);
        scm->svcs[scm->count].ndeps++;
    }

    scm->count++;
    compute_order(scm);

    NTB0_OK("SVC-001", "service '%s' created in memory (start %u, type 0x%x, %d dependency/ies) — not written to the hive",
            name, start, type, scm->svcs[scm->count - 1].ndeps);
    return NTB0_SCM_OK;
}

/* ------------------------------------------------------------------ */
/* أسماء مقروءة                                                       */
/* ------------------------------------------------------------------ */
const char *ntb0_scm_state_name(int state)
{
    switch (state) {
    case NTB0_SVC_DISABLED:      return "DISABLED";
    case NTB0_SVC_STOPPED:       return "STOPPED";
    case NTB0_SVC_START_PENDING: return "START_PENDING";
    case NTB0_SVC_STOP_PENDING:  return "STOP_PENDING";
    case NTB0_SVC_RUNNING:       return "RUNNING";
    case NTB0_SVC_PAUSED:        return "PAUSED";
    default:                     return "UNKNOWN";
    }
}

const char *ntb0_scm_start_name(uint32_t start)
{
    switch (start) {
    case NTB0_SVC_START_BOOT:     return "boot";
    case NTB0_SVC_START_SYSTEM:   return "system";
    case NTB0_SVC_START_AUTO:     return "auto";
    case NTB0_SVC_START_DEMAND:   return "demand";
    case NTB0_SVC_START_DISABLED: return "disabled";
    default:                      return "unknown";
    }
}

const char *ntb0_scm_type_name(uint32_t type)
{
    switch (type & 0xFFu) {
    case NTB0_SVC_TYPE_KERNEL_DRIVER: return "kernel driver";
    case NTB0_SVC_TYPE_FILE_SYSTEM:    return "file system driver";
    case NTB0_SVC_TYPE_ADAPTER:        return "adapter";
    case NTB0_SVC_TYPE_RECOGNIZER:     return "recognizer driver";
    case NTB0_SVC_TYPE_WIN32_OWN:      return "win32 own process";
    case NTB0_SVC_TYPE_WIN32_SHARE:    return "win32 share process";
    default:                           return "unknown";
    }
}
