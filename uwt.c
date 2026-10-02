#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lzma/LzmaDec.h"
#include "lzma/LzmaEnc.h"

#define APP_NAME "UWT Unity Web Tool"
#define VERSION "2.1.0"

#define UW_FORMAT     3
#define UW_UNITY_VER  "5.x.x"
#define UW_UNITY_REV  "5.6.7f1"

static void *SzAlloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return malloc(size);
}

static void SzFree(ISzAllocPtr p, void *address)
{
    (void)p;
    free(address);
}

static ISzAlloc g_Alloc = { SzAlloc, SzFree };

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void write_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static int read_cstring(const uint8_t *buf, size_t len, size_t *pos,
                        char *out, size_t out_sz)
{
    size_t i = *pos;
    size_t n = 0;
    if (i >= len)
        return -1;
    while (i < len && buf[i] != 0) {
        if (n + 1 < out_sz)
            out[n++] = (char)buf[i];
        i++;
    }
    if (i >= len)
        return -1;
    out[n] = 0;
    *pos = i + 1;
    return 0;
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    size_t i;
    if (ls < lf)
        return 0;
    for (i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static void make_default_out(const char *in, const char *suffix,
                             char *out, size_t out_sz)
{
    const char *slash = strrchr(in, '\\');
    const char *slash2 = strrchr(in, '/');
    const char *base = in;
    char tmp[1024];

    if (slash && slash2)
        base = (slash > slash2) ? slash + 1 : slash2 + 1;
    else if (slash)
        base = slash + 1;
    else if (slash2)
        base = slash2 + 1;

    strncpy(tmp, base, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    {
        char *d = strrchr(tmp, '.');
        if (d && ends_with_ci(tmp, ".unity3d"))
            *d = 0;
    }
    snprintf(out, out_sz, "%s%s", tmp, suffix);
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f;
    long sz;
    uint8_t *buf;

    f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        perror("ftell");
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!buf) {
        fprintf(stderr, "Out of memory reading %s (%ld bytes)\n", path, sz);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "Short read: %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    if (fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "Short write: %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int lzma_alone_decompress(const uint8_t *src, size_t src_len,
                                 uint8_t **out_buf, size_t *out_len)
{
    const uint8_t *props;
    uint64_t unpack_size;
    SizeT dest_len, src_used;
    ELzmaStatus status;
    SRes res;
    uint8_t *dest;

    if (src_len < 13) {
        fprintf(stderr, "LZMA stream too small (%zu bytes)\n", src_len);
        return -1;
    }

    props = src;
    unpack_size = read_le64(src + 5);
    if (unpack_size == 0 || unpack_size == (uint64_t)-1 || unpack_size > (uint64_t)8 * 1024 * 1024 * 1024ULL) {
        fprintf(stderr, "Suspicious uncompressed size: %llu\n",
                (unsigned long long)unpack_size);
        return -1;
    }

    dest = (uint8_t *)malloc((size_t)unpack_size);
    if (!dest) {
        fprintf(stderr, "Out of memory for %llu byte output\n",
                (unsigned long long)unpack_size);
        return -1;
    }

    dest_len = (SizeT)unpack_size;
    src_used = (SizeT)(src_len - 13);
    printf("LZMA props=0x%02X dict=%u unpack=%llu compressed=%zu\n",
           props[0],
           (unsigned)(props[1] | (props[2] << 8) | (props[3] << 16) | ((unsigned)props[4] << 24)),
           (unsigned long long)unpack_size,
           (size_t)src_used);

    res = LzmaDecode(dest, &dest_len,
                     src + 13, &src_used,
                     props, 5,
                     LZMA_FINISH_END, &status, &g_Alloc);
    if (res != SZ_OK) {
        fprintf(stderr, "LzmaDecode failed: %d (status=%d)\n", (int)res, (int)status);
        free(dest);
        return -1;
    }
    if (dest_len != (SizeT)unpack_size) {
        fprintf(stderr, "Warning: decoded %zu bytes, header said %llu\n",
                (size_t)dest_len, (unsigned long long)unpack_size);
    }

    *out_buf = dest;
    *out_len = (size_t)dest_len;
    return 0;
}

static int parse_unity_legacy(const uint8_t *buf, size_t len,
                              char *sig_out, size_t sig_sz,
                              uint32_t *format_out,
                              uint32_t *header_size_out,
                              uint32_t *uncomp_hint_out,
                              int verbose)
{
    size_t pos = 0;
    char unity_ver[64], unity_rev[64];
    uint32_t format, min_streamed, header_size, n_download, level_count;
    uint32_t i, complete_size, file_info_header_size;
    uint32_t uncomp_hint = 0;

    if (read_cstring(buf, len, &pos, sig_out, sig_sz) != 0)
        return -1;
    if (strcmp(sig_out, "UnityWeb") != 0 && strcmp(sig_out, "UnityRaw") != 0)
        return -1;
    if (pos + 4 > len)
        return -1;

    format = read_be32(buf + pos);
    pos += 4;
    if (read_cstring(buf, len, &pos, unity_ver, sizeof(unity_ver)) != 0)
        return -1;
    if (read_cstring(buf, len, &pos, unity_rev, sizeof(unity_rev)) != 0)
        return -1;

    if (pos + 16 > len)
        return -1;
    min_streamed = read_be32(buf + pos); pos += 4;
    header_size = read_be32(buf + pos); pos += 4;
    n_download = read_be32(buf + pos); pos += 4;
    level_count = read_be32(buf + pos); pos += 4;

    if (verbose) {
        printf("Signature : %s\n", sig_out);
        printf("Format    : %u\n", format);
        printf("Unity     : %s / %s\n", unity_ver, unity_rev);
        printf("HeaderSize: %u\n", header_size);
        printf("Levels    : %u (downloadBeforeStream=%u)\n", level_count, n_download);
        printf("MinStream : %u\n", min_streamed);
    }

    if (level_count > 1024 || pos + (size_t)level_count * 8 > len)
        return -1;

    for (i = 0; i < level_count; i++) {
        uint32_t csz = read_be32(buf + pos); pos += 4;
        uint32_t usz = read_be32(buf + pos); pos += 4;
        if (verbose)
            printf("  level[%u]: compressed=%u uncompressed=%u\n", i, csz, usz);
        if (usz > uncomp_hint)
            uncomp_hint = usz;
    }

    if (format >= 2) {
        if (pos + 4 > len)
            return -1;
        complete_size = read_be32(buf + pos); pos += 4;
        if (verbose)
            printf("Complete  : %u\n", complete_size);
    }
    if (format >= 3) {
        if (pos + 4 > len)
            return -1;
        file_info_header_size = read_be32(buf + pos); pos += 4;
        if (verbose)
            printf("FileInfoHdr: %u\n", file_info_header_size);
    }
    (void)pos;

    if (header_size == 0 || (size_t)header_size >= len) {
        fprintf(stderr, "Invalid headerSize %u for file of %zu bytes\n",
                header_size, len);
        return -1;
    }

    *format_out = format;
    *header_size_out = header_size;
    *uncomp_hint_out = uncomp_hint;
    return 0;
}

static int lzma_alone_compress(const uint8_t *src, size_t src_len, int level,
                               uint8_t **out, size_t *out_len)
{
    CLzmaEncProps ep;
    uint8_t enc_props[LZMA_PROPS_SIZE];
    SizeT props_size = LZMA_PROPS_SIZE;
    SizeT dest_len;
    uint8_t *dest;
    SRes res;

    LzmaEncProps_Init(&ep);
    ep.level = level;
    ep.lc = 3;
    ep.lp = 0;
    ep.pb = 2;
    ep.dictSize = 1u << 23;
    ep.writeEndMark = 0;
    ep.numThreads = 1;
    LzmaEncProps_Normalize(&ep);

    printf("Encoding: lc=3 lp=0 pb=2 dict=%u level=%d, %zu bytes in\n",
           (unsigned)ep.dictSize, level, src_len);

    dest_len = (SizeT)(src_len + src_len / 3 + 128);
    dest = (uint8_t *)malloc(13 + (size_t)dest_len);
    if (!dest) {
        fprintf(stderr, "Out of memory for encoder output\n");
        return -1;
    }

    res = LzmaEncode(dest + 13, &dest_len, src, (SizeT)src_len, &ep,
                     enc_props, &props_size, 0, NULL, &g_Alloc, &g_Alloc);
    if (res != SZ_OK) {
        fprintf(stderr, "LzmaEncode failed: %d\n", (int)res);
        free(dest);
        return -1;
    }

    memcpy(dest, enc_props, 5);
    write_le64(dest + 5, (uint64_t)src_len);
    *out = dest;
    *out_len = 13 + (size_t)dest_len;
    return 0;
}

static uint32_t guess_file_info_size(const uint8_t *d, size_t n)
{
    size_t pos = 4;
    uint32_t count, i, first = 0;
    char name[256];

    if (n < 4)
        return 0;
    count = read_be32(d);
    if (count == 0 || count > 100000)
        return 0;
    for (i = 0; i < count; i++) {
        if (read_cstring(d, n, &pos, name, sizeof(name)) != 0 || pos + 8 > n)
            return 0;
        if (i == 0)
            first = read_be32(d + pos);
        pos += 8;
    }
    if (first < pos || first > n)
        return 0;
    return first;
}

static size_t unityweb_header_len(void)
{
    return 9 + 4 + sizeof(UW_UNITY_VER) + sizeof(UW_UNITY_REV) + 16 + 8
           + (UW_FORMAT >= 2 ? 4 : 0) + (UW_FORMAT >= 3 ? 4 : 0);
}

static void write_unityweb_header(uint8_t *h, uint32_t payload_len,
                                  uint32_t uncomp_len, uint32_t total_len,
                                  uint32_t file_info_size)
{
    size_t pos = 0;

    memcpy(h + pos, "UnityWeb", 9);
    pos += 9;
    write_be32(h + pos, UW_FORMAT);
    pos += 4;
    memcpy(h + pos, UW_UNITY_VER, sizeof(UW_UNITY_VER));
    pos += sizeof(UW_UNITY_VER);
    memcpy(h + pos, UW_UNITY_REV, sizeof(UW_UNITY_REV));
    pos += sizeof(UW_UNITY_REV);

    write_be32(h + pos, total_len);
    pos += 4;
    write_be32(h + pos, (uint32_t)unityweb_header_len());
    pos += 4;
    write_be32(h + pos, 1);
    pos += 4;
    write_be32(h + pos, 1);
    pos += 4;
    write_be32(h + pos, payload_len);
    pos += 4;
    write_be32(h + pos, uncomp_len);
    pos += 4;
    if (UW_FORMAT >= 2) {
        write_be32(h + pos, total_len);
        pos += 4;
    }
    if (UW_FORMAT >= 3) {
        write_be32(h + pos, file_info_size);
        pos += 4;
    }
}

static int do_compress(const char *in_path, const char *out_path, int level)
{
    uint8_t *data = NULL, *lz = NULL, *final_buf = NULL;
    size_t data_len = 0, lz_len = 0, hdr_len = unityweb_header_len();
    size_t final_len;
    uint32_t fi_size;
    int rc = 1;

    data = read_file(in_path, &data_len);
    if (!data)
        goto done;
    printf("Input : %s (%zu bytes)\n", in_path, data_len);

    if (data_len == 0) {
        fprintf(stderr, "Nothing to compress\n");
        goto done;
    }

    if (lzma_alone_compress(data, data_len, level, &lz, &lz_len) != 0)
        goto done;
    printf("Compressed to %zu bytes (incl. 13-byte LZMA header)\n", lz_len);

    final_len = hdr_len + lz_len;
    if (data_len > 0xFFFFFFFFu || final_len > 0xFFFFFFFFu) {
        fprintf(stderr, "File too large for a UnityWeb header (32-bit sizes)\n");
        goto done;
    }

    final_buf = (uint8_t *)malloc(final_len);
    if (!final_buf) {
        fprintf(stderr, "Out of memory\n");
        goto done;
    }

    fi_size = guess_file_info_size(data, data_len);
    write_unityweb_header(final_buf, (uint32_t)lz_len, (uint32_t)data_len,
                          (uint32_t)final_len, fi_size);
    memcpy(final_buf + hdr_len, lz, lz_len);
    printf("Header: UnityWeb format %d, %s / %s, %zu bytes\n",
           UW_FORMAT, UW_UNITY_VER, UW_UNITY_REV, hdr_len);

    printf("Output: %s (%zu bytes)\n", out_path, final_len);
    if (write_file(out_path, final_buf, final_len) != 0)
        goto done;
    printf("Done.\n");
    rc = 0;

done:
    free(data);
    free(lz);
    free(final_buf);
    return rc;
}

static int do_decompress(const char *in_path, const char *out_path)
{
    uint8_t *file = NULL;
    size_t file_len = 0;
    uint8_t *out = NULL;
    size_t out_len = 0;
    char sig[32];
    uint32_t format = 0, header_size = 0, uncomp_hint = 0;
    int rc = 1;

    printf("Input : %s\n", in_path);
    printf("Output: %s\n", out_path);

    file = read_file(in_path, &file_len);
    if (!file)
        return 1;
    printf("Size  : %zu bytes\n", file_len);

    if (file_len >= 8 && memcmp(file, "UnityFS", 7) == 0) {
        fprintf(stderr,
                "This is a UnityFS bundle, not UnityWeb.\n"
                "Your Test.bms targets UnityFS; this tool is for UnityWeb LZMA.\n");
        goto done;
    }

    if (file_len >= 9 &&
        (memcmp(file, "UnityWeb", 8) == 0 || memcmp(file, "UnityRaw", 8) == 0)) {

        if (parse_unity_legacy(file, file_len, sig, sizeof(sig),
                               &format, &header_size, &uncomp_hint, 1) != 0) {
            fprintf(stderr, "Failed to parse UnityWeb/UnityRaw header\n");
            goto done;
        }

        if (strcmp(sig, "UnityRaw") == 0) {
            printf("Already UnityRaw (uncompressed). Copying payload after header...\n");
            out_len = file_len - header_size;
            out = (uint8_t *)malloc(out_len);
            if (!out) {
                fprintf(stderr, "Out of memory\n");
                goto done;
            }
            memcpy(out, file + header_size, out_len);
        } else {
            printf("Decompressing LZMA payload at offset %u (%zu bytes)...\n",
                   header_size, file_len - header_size);
            if (lzma_alone_decompress(file + header_size, file_len - header_size,
                                      &out, &out_len) != 0)
                goto done;
            if (uncomp_hint && out_len != uncomp_hint) {
                printf("Note: level uncompressed hint was %u, got %zu\n",
                       uncomp_hint, out_len);
            }
        }
    } else if (file_len >= 13 && file[0] <= 225) {
        printf("No Unity signature; trying raw LZMA-alone...\n");
        if (lzma_alone_decompress(file, file_len, &out, &out_len) != 0)
            goto done;
    } else {
        fprintf(stderr, "Unrecognized format (expected UnityWeb / UnityRaw / LZMA)\n");
        goto done;
    }

    printf("Writing %zu bytes...\n", out_len);
    if (write_file(out_path, out, out_len) != 0)
        goto done;

    printf("Done.\n");
    if (out_len >= 4)
        printf("Output magic/preview: %02X %02X %02X %02X ...\n",
               out[0], out[1], out[2], out[3]);
    rc = 0;

done:
    free(file);
    free(out);
    return rc;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            APP_NAME " v%s\n\n"
            "Decompress a UnityWeb (.unity3d) bundle:\n"
            "  %s -d <input.unity3d> [output]\n\n"
            "Compress a decompressed payload into a UnityWeb bundle:\n"
            "  %s -c <decompressed> [output] [-l N]\n\n"
            "Options:\n"
            "  -d, --decompress      decompress mode\n"
            "  -c, --compress        compress mode\n"
            "  -l, --level N         LZMA level 0-9 (default 5), used with -c\n\n"
            "Default outputs: <name>_decompressed.unity3d / <name>_compressed.unity3d\n"
            "Note: UnityFS bundles need a different tool; this targets UnityWeb.\n",
            VERSION, argv0, argv0);
}

int main(int argc, char **argv)
{
    const char *pos_args[2] = { NULL, NULL };
    int npos = 0;
    int compress = 0, decompress = 0, level = 5;
    int i;
    char out_path_buf[1024];
    const char *out_path;

    if (argc < 2) {
        usage("uwt");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(a, "-d") || !strcmp(a, "--decompress")) {
            decompress = 1;
        } else if (!strcmp(a, "-c") || !strcmp(a, "--compress")) {
            compress = 1;
        } else if ((!strcmp(a, "-l") || !strcmp(a, "--level")) && i + 1 < argc) {
            level = atoi(argv[++i]);
            if (level < 0) level = 0;
            if (level > 9) level = 9;
        } else if (a[0] == '-' && a[1] != 0) {
            fprintf(stderr, "Unknown or incomplete option: %s\n\n", a);
            usage(argv[0]);
            return 1;
        } else if (npos < 2) {
            pos_args[npos++] = a;
        } else {
            fprintf(stderr, "Too many arguments\n\n");
            usage(argv[0]);
            return 1;
        }
    }

    if (compress == decompress) {
        fprintf(stderr, "Specify exactly one of -d or -c\n\n");
        usage(argv[0]);
        return 1;
    }

    if (npos < 1) {
        usage(argv[0]);
        return 1;
    }

    if (compress) {
        if (npos >= 2) {
            out_path = pos_args[1];
        } else {
            make_default_out(pos_args[0], "_compressed.unity3d",
                             out_path_buf, sizeof(out_path_buf));
            out_path = out_path_buf;
        }
        return do_compress(pos_args[0], out_path, level);
    }

    if (npos >= 2) {
        out_path = pos_args[1];
    } else {
        make_default_out(pos_args[0], "_decompressed.unity3d",
                         out_path_buf, sizeof(out_path_buf));
        out_path = out_path_buf;
    }
    return do_decompress(pos_args[0], out_path);
}