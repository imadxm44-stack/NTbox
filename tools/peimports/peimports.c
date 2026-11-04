/*
 * peimports — مستخرج Imports من ملف PE (32/64)
 * الهدف: M3 — معرفة بالضبط ما تستدعيه ملفات ويندوز (7/10/11)
 * قبل ترجمتها. بحث محلي فقط (§8.1): لا نشر، لا تخزين في المستودع.
 *
 * Build: gcc -O2 -o peimports peimports.c
 * Usage: peimports <file.sys|.dll|.exe> [--unique]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static u8 *data;
static size_t dsize;

static int rva_to_off(u32 rva, u32 *out)
{
    /* parse sections */
    u32 e_lfanew;
    u16 nsec;
    size_t i;
    u8 *nt, *sec;

    if (dsize < 0x40) return -1;
    if (data[0] != 'M' || data[1] != 'Z') return -1;
    e_lfanew = *(u32 *)(data + 0x3C);
    if (e_lfanew + 0x18 > dsize) return -1;
    nt = data + e_lfanew;
    if (nt[0] != 'P' || nt[1] != 'E') return -1;
    nsec = *(u16 *)(nt + 6);
    sec = nt + 24 + *(u16 *)(nt + 20); /* after optional header */
    for (i = 0; i < nsec; i++) {
        u8 *s = sec + i * 40;
        u32 va = *(u32 *)(s + 12);
        u32 vsz = *(u32 *)(s + 8);
        u32 raw = *(u32 *)(s + 20);
        u32 rsz = *(u32 *)(s + 16);
        if (rva >= va && rva < va + (vsz ? vsz : rsz)) {
            *out = raw + (rva - va);
            return 0;
        }
    }
    return -1;
}

static char *get_str(u32 rva)
{
    u32 off;
    if (rva_to_off(rva, &off) != 0) return NULL;
    if (off >= dsize) return NULL;
    return (char *)(data + off);
}

int main(int argc, char **argv)
{
    FILE *f;
    int unique = 0;
    size_t i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <pe-file> [--unique]\n", argv[0]);
        return 1;
    }
    for (i = 2; i < (size_t)argc; i++)
        if (!strcmp(argv[i], "--unique")) unique = 1;

    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    dsize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(dsize);
    if (!data || fread(data, 1, dsize, f) != dsize) { perror("read"); return 1; }
    fclose(f);

    /* NT headers */
    u32 e_lfanew = *(u32 *)(data + 0x3C);
    u8 *nt = data + e_lfanew;
    u16 magic = *(u16 *)(nt + 24);
    int is64 = (magic == 0x20B);
    u16 nsec = *(u16 *)(nt + 6);
    u32 optsize = *(u16 *)(nt + 20);
    u8 *opt = nt + 24;
    /* DataDirectory: import table is index 1; offset within optional header: 96 (32) or 112 (64) */
    u32 import_rva, import_size;
    u32 dd_off = is64 ? 112 : 96;
    import_rva = *(u32 *)(opt + dd_off + 8);   /* DataDirectory[1] = Import */
    import_size = *(u32 *)(opt + dd_off + 12);

    if (!import_rva) {
        printf("(no import table)\n");
        return 0;
    }

    /* walk import descriptors */
    u32 desc_off;
    if (rva_to_off(import_rva, &desc_off) != 0) { fprintf(stderr, "bad import rva\n"); return 1; }

    /* collect unique names if requested */
    /* We'll just print per-DLL */
    size_t total = 0;
    /* iterate */
    u32 cur = desc_off;
    while (1) {
        u32 orig_thunk, first_thunk, name_rva;
        /* IMAGE_IMPORT_DESCRIPTOR: 20 bytes */
        if (cur + 20 > dsize) break;
        orig_thunk = *(u32 *)(data + cur);
        /* TimeDateStamp at +4, ForwarderChain +8 */
        name_rva = *(u32 *)(data + cur + 12);
        first_thunk = *(u32 *)(data + cur + 16);
        if (name_rva == 0 && first_thunk == 0 && orig_thunk == 0) break;

        char *dll = get_str(name_rva);
        printf("== %s ==\n", dll ? dll : "(?)");

        u32 thunk_rva = orig_thunk ? orig_thunk : first_thunk;
        u32 thunk_off;
        if (rva_to_off(thunk_rva, &thunk_off) == 0) {
            while (1) {
                u64 thunk;
                if (is64) {
                    if (thunk_off + 8 > dsize) break;
                    thunk = *(u64 *)(data + thunk_off);
                    thunk_off += 8;
                } else {
                    if (thunk_off + 4 > dsize) break;
                    thunk = *(u32 *)(data + thunk_off);
                    thunk_off += 4;
                }
                if (thunk == 0) break;
                if (thunk & (is64 ? 0x8000000000000000ULL : 0x80000000U)) {
                    printf("   (ordinal %llu)\n", (unsigned long long)(thunk & 0xFFFF));
                } else {
                    u32 ibn_off;
                    if (rva_to_off((u32)thunk, &ibn_off) == 0 && ibn_off + 2 < dsize) {
                        char *name = (char *)(data + ibn_off + 2);
                        printf("   %s\n", name);
                        total++;
                    }
                }
            }
        }
        cur += 20;
    }
    fprintf(stderr, "\ntotal named imports: %zu\n", total);
    return 0;
}
