#define _POSIX_C_SOURCE 200809L
#include "classfile.h"

/* ════════════════════════════════════════════════════════════
 *  Internal byte-stream reader
 * ════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       pos;
} ByteStream;

static CFResult bs_read_u8(ByteStream *bs, uint8_t *out) {
    if (bs->pos >= bs->len) return CF_ERR_TRUNCATED;
    *out = bs->data[bs->pos++];
    return CF_OK;
}

static CFResult bs_read_u16(ByteStream *bs, uint16_t *out) {
    if (bs->pos + 2 > bs->len) return CF_ERR_TRUNCATED;
    *out = (uint16_t)((bs->data[bs->pos] << 8) | bs->data[bs->pos + 1]);
    bs->pos += 2;
    return CF_OK;
}

static CFResult bs_read_u32(ByteStream *bs, uint32_t *out) {
    if (bs->pos + 4 > bs->len) return CF_ERR_TRUNCATED;
    *out = ((uint32_t)bs->data[bs->pos    ] << 24) |
           ((uint32_t)bs->data[bs->pos + 1] << 16) |
           ((uint32_t)bs->data[bs->pos + 2] <<  8) |
            (uint32_t)bs->data[bs->pos + 3];
    bs->pos += 4;
    return CF_OK;
}

static CFResult bs_skip(ByteStream *bs, uint32_t n) {
    if (bs->pos + n > bs->len) return CF_ERR_TRUNCATED;
    bs->pos += n;
    return CF_OK;
}

/* Read n bytes into a heap-allocated buffer */
static CFResult bs_read_bytes(ByteStream *bs, uint32_t n, uint8_t **out) {
    if (bs->pos + n > bs->len) return CF_ERR_TRUNCATED;
    *out = (uint8_t *)malloc(n);
    if (!*out) return CF_ERR_IO;
    memcpy(*out, bs->data + bs->pos, n);
    bs->pos += n;
    return CF_OK;
}

/* ════════════════════════════════════════════════════════════
 *  Constant pool helpers
 * ════════════════════════════════════════════════════════════ */

const char *cp_utf8(const ClassFile *cf, uint16_t index) {
    if (index == 0 || index >= cf->constant_pool_count) return "";
    CPEntry *e = &cf->constant_pool[index];
    if (e->tag != CP_UTF8) return "";
    return e->utf8.bytes;
}

/* ════════════════════════════════════════════════════════════
 *  Parse constant pool
 * ════════════════════════════════════════════════════════════ */

static CFResult parse_constant_pool(ByteStream *bs,
                                    uint16_t count,
                                    CPEntry *cp)
{
    CFResult r;
    /* index 0 is unused; entries are 1-based */
    for (uint16_t i = 1; i < count; i++) {
        uint8_t tag;
        r = bs_read_u8(bs, &tag); if (r) return r;
        cp[i].tag = tag;

        switch (tag) {
        case CP_UTF8: {
            uint16_t len;
            r = bs_read_u16(bs, &len); if (r) return r;
            cp[i].utf8.length = len;
            cp[i].utf8.bytes  = (char *)malloc(len + 1);
            if (!cp[i].utf8.bytes) return CF_ERR_IO;
            if (bs->pos + len > bs->len) return CF_ERR_TRUNCATED;
            memcpy(cp[i].utf8.bytes, bs->data + bs->pos, len);
            cp[i].utf8.bytes[len] = '\0';
            bs->pos += len;
            break;
        }
        case CP_INTEGER: {
            uint32_t v; r = bs_read_u32(bs, &v); if (r) return r;
            cp[i].integer.value = (int32_t)v;
            break;
        }
        case CP_FLOAT:
            r = bs_skip(bs, 4); if (r) return r; break;
        case CP_LONG:
        case CP_DOUBLE:
            r = bs_skip(bs, 8); if (r) return r;
            /* long/double take two CP slots */
            i++;
            cp[i].tag = tag;
            break;
        case CP_CLASS:
            r = bs_read_u16(bs, &cp[i].cls.name_index); if (r) return r;
            break;
        case CP_STRING:
            r = bs_read_u16(bs, &cp[i].string.string_index); if (r) return r;
            break;
        case CP_FIELDREF:
        case CP_METHODREF:
        case CP_INTERFACEMETHODREF:
            r = bs_read_u16(bs, &cp[i].ref.class_index); if (r) return r;
            r = bs_read_u16(bs, &cp[i].ref.name_and_type_index); if (r) return r;
            break;
        case CP_NAMEANDTYPE:
            r = bs_read_u16(bs, &cp[i].nat.name_index); if (r) return r;
            r = bs_read_u16(bs, &cp[i].nat.descriptor_index); if (r) return r;
            break;
        default:
            /* Unknown tag — we can't continue safely */
            fprintf(stderr, "[classfile] unknown CP tag %d at index %d\n", tag, i);
            return CF_ERR_UNSUPPORTED;
        }
    }
    return CF_OK;
}

/* ════════════════════════════════════════════════════════════
 *  Parse attributes — we only care about "Code"
 * ════════════════════════════════════════════════════════════ */

static CFResult parse_code_attribute(ByteStream *bs,
                                     MethodInfo *m)
{
    CFResult r;
    r = bs_read_u16(bs, &m->max_stack);  if (r) return r;
    r = bs_read_u16(bs, &m->max_locals); if (r) return r;

    uint32_t code_len;
    r = bs_read_u32(bs, &code_len); if (r) return r;
    m->code_length = code_len;

    r = bs_read_bytes(bs, code_len, &m->code); if (r) return r;

    /* exception table */
    uint16_t ex_count;
    r = bs_read_u16(bs, &ex_count); if (r) return r;
    m->exception_table_length = ex_count;
    if (ex_count > 0) {
        m->exception_table = (ExceptionEntry *)calloc(ex_count, sizeof(ExceptionEntry));
        if (!m->exception_table) return CF_ERR_IO;
        for (uint16_t i = 0; i < ex_count; i++) {
            r = bs_read_u16(bs, &m->exception_table[i].start_pc);   if (r) return r;
            r = bs_read_u16(bs, &m->exception_table[i].end_pc);     if (r) return r;
            r = bs_read_u16(bs, &m->exception_table[i].handler_pc); if (r) return r;
            r = bs_read_u16(bs, &m->exception_table[i].catch_type); if (r) return r;
        }
    }

    /* skip nested attributes inside Code */
    uint16_t attr_count;
    r = bs_read_u16(bs, &attr_count); if (r) return r;
    for (uint16_t i = 0; i < attr_count; i++) {
        r = bs_skip(bs, 2); if (r) return r; /* name_index */
        uint32_t alen;
        r = bs_read_u32(bs, &alen); if (r) return r;
        r = bs_skip(bs, alen); if (r) return r;
    }
    return CF_OK;
}

static CFResult parse_method_attributes(ByteStream *bs,
                                        MethodInfo *m,
                                        const ClassFile *cf)
{
    CFResult r;
    uint16_t attr_count;
    r = bs_read_u16(bs, &attr_count); if (r) return r;

    for (uint16_t i = 0; i < attr_count; i++) {
        uint16_t name_idx;
        r = bs_read_u16(bs, &name_idx); if (r) return r;
        uint32_t attr_len;
        r = bs_read_u32(bs, &attr_len); if (r) return r;

        const char *attr_name = cp_utf8(cf, name_idx);
        if (strcmp(attr_name, "Code") == 0) {
            r = parse_code_attribute(bs, m); if (r) return r;
        } else {
            r = bs_skip(bs, attr_len); if (r) return r;
        }
    }
    return CF_OK;
}

/* ════════════════════════════════════════════════════════════
 *  Main parser
 * ════════════════════════════════════════════════════════════ */

CFResult classfile_parse(const uint8_t *data, uint32_t len,
                         ClassFile **out)
{
    CFResult r;
    ByteStream bs = { data, len, 0 };

    /* magic */
    uint32_t magic;
    r = bs_read_u32(&bs, &magic); if (r) return r;
    if (magic != 0xCAFEBABE) return CF_ERR_MAGIC;

    /* minor / major version */
    r = bs_skip(&bs, 4); if (r) return r;

    ClassFile *cf = (ClassFile *)calloc(1, sizeof(ClassFile));
    if (!cf) return CF_ERR_IO;

    /* constant pool */
    r = bs_read_u16(&bs, &cf->constant_pool_count); if (r) goto fail;
    cf->constant_pool = (CPEntry *)calloc(cf->constant_pool_count, sizeof(CPEntry));
    if (!cf->constant_pool) { r = CF_ERR_IO; goto fail; }
    r = parse_constant_pool(&bs, cf->constant_pool_count, cf->constant_pool);
    if (r) goto fail;

    /* access flags */
    r = bs_read_u16(&bs, &cf->access_flags); if (r) goto fail;

    /* this_class -> resolve class name */
    uint16_t this_class;
    r = bs_read_u16(&bs, &this_class); if (r) goto fail;
    if (cf->constant_pool[this_class].tag == CP_CLASS) {
        uint16_t name_idx = cf->constant_pool[this_class].cls.name_index;
        const char *cname = cp_utf8(cf, name_idx);
        cf->class_name = strdup(cname);
    }

    /* super_class */
    r = bs_skip(&bs, 2); if (r) goto fail;

    /* interfaces */
    uint16_t iface_count;
    r = bs_read_u16(&bs, &iface_count); if (r) goto fail;
    r = bs_skip(&bs, (uint32_t)iface_count * 2); if (r) goto fail;

    /* fields — skip each field's attributes */
    uint16_t field_count;
    r = bs_read_u16(&bs, &field_count); if (r) goto fail;
    for (uint16_t i = 0; i < field_count; i++) {
        r = bs_skip(&bs, 6); if (r) goto fail; /* access, name, descriptor */
        uint16_t attr_count;
        r = bs_read_u16(&bs, &attr_count); if (r) goto fail;
        for (uint16_t j = 0; j < attr_count; j++) {
            r = bs_skip(&bs, 2); if (r) goto fail;
            uint32_t alen; r = bs_read_u32(&bs, &alen); if (r) goto fail;
            r = bs_skip(&bs, alen); if (r) goto fail;
        }
    }

    /* methods */
    r = bs_read_u16(&bs, &cf->methods_count); if (r) goto fail;
    cf->methods = (MethodInfo *)calloc(cf->methods_count, sizeof(MethodInfo));
    if (!cf->methods) { r = CF_ERR_IO; goto fail; }

    for (uint16_t i = 0; i < cf->methods_count; i++) {
        MethodInfo *m = &cf->methods[i];
        r = bs_read_u16(&bs, &m->access_flags); if (r) goto fail;
        uint16_t name_idx, desc_idx;
        r = bs_read_u16(&bs, &name_idx); if (r) goto fail;
        r = bs_read_u16(&bs, &desc_idx); if (r) goto fail;
        m->name       = strdup(cp_utf8(cf, name_idx));
        m->descriptor = strdup(cp_utf8(cf, desc_idx));
        r = parse_method_attributes(&bs, m, cf); if (r) goto fail;
    }

    *out = cf;
    return CF_OK;

fail:
    classfile_free(cf);
    return r;
}

/* ════════════════════════════════════════════════════════════
 *  Free a ClassFile
 * ════════════════════════════════════════════════════════════ */

void classfile_free(ClassFile *cf) {
    if (!cf) return;
    if (cf->constant_pool) {
        for (uint16_t i = 1; i < cf->constant_pool_count; i++) {
            if (cf->constant_pool[i].tag == CP_UTF8)
                free(cf->constant_pool[i].utf8.bytes);
        }
        free(cf->constant_pool);
    }
    if (cf->methods) {
        for (uint16_t i = 0; i < cf->methods_count; i++) {
            free(cf->methods[i].name);
            free(cf->methods[i].descriptor);
            free(cf->methods[i].code);
            free(cf->methods[i].exception_table);
        }
        free(cf->methods);
    }
    free(cf->class_name);
    free(cf);
}

/* ════════════════════════════════════════════════════════════
 *  Load a .class file from disk
 * ════════════════════════════════════════════════════════════ */

CFResult classfile_load(const char *path, ClassFile **out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return CF_ERR_IO;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    
    /* ─── FIX: Check for ftell error before allocating ─── */
    if (sz < 0) { 
        fclose(fp); 
        return CF_ERR_IO; 
    }
    
    rewind(fp);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return CF_ERR_IO; }

    if ((long)fread(buf, 1, (size_t)sz, fp) != sz) {
        free(buf); fclose(fp); return CF_ERR_IO;
    }
    fclose(fp);

    CFResult r = classfile_parse(buf, (uint32_t)sz, out);
    free(buf);
    return r;
}

/* ════════════════════════════════════════════════════════════
 *  Method lookup
 * ════════════════════════════════════════════════════════════ */

MethodInfo *classfile_find_method(const ClassFile *cf,
                                  const char *name,
                                  const char *descriptor)
{
    for (uint16_t i = 0; i < cf->methods_count; i++) {
        MethodInfo *m = &cf->methods[i];
        if (strcmp(m->name, name) != 0) continue;
        if (descriptor && strcmp(m->descriptor, descriptor) != 0) continue;
        return m;
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════
 *  Class registry
 * ════════════════════════════════════════════════════════════ */

void registry_init(ClassRegistry *reg) {
    memset(reg, 0, sizeof(ClassRegistry));
}

CFResult registry_add(ClassRegistry *reg, ClassFile *cf) {
    if (reg->count >= MAX_CLASSES) return CF_ERR_REGISTRY_FULL;
    reg->classes[reg->count++] = cf;
    return CF_OK;
}

ClassFile *registry_find(ClassRegistry *reg, const char *class_name) {
    for (int i = 0; i < reg->count; i++) {
        if (reg->classes[i]->class_name &&
            strcmp(reg->classes[i]->class_name, class_name) == 0)
            return reg->classes[i];
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════
 *  Minimal ZIP / JAR parser
 *  Only reads the Local File Headers (no central directory needed
 *  for sequential extraction of stored/deflated entries).
 *  We use the Central Directory to get file names & offsets,
 *  and only load entries whose name ends in ".class".
 *
 *  ZIP format reference: APPNOTE.TXT §4.3
 * ════════════════════════════════════════════════════════════ */

#define ZIP_LOCAL_SIG   0x04034B50u
#define ZIP_CENTRAL_SIG 0x02014B50u
#define ZIP_EOCD_SIG    0x06054B50u

static uint32_t zip_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint16_t zip_u16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1]<<8);
}


static int find_eocd(const uint8_t *data, uint32_t len,
                     uint32_t *cd_offset, uint16_t *cd_entries) // Added * to cd_entries
{
    if (len < 22) return 0;
    for (int i = (int)len - 22; i >= 0; i--) {
        if (zip_u32le(data + i) == ZIP_EOCD_SIG) {
            *cd_entries = zip_u16le(data + i + 10);
            *cd_offset = zip_u32le(data + i + 16);
            return 1;
        }
    }
    return 0;
}

/* Very small inflate stub — we only support STORED (method=0) entries.
   For a GSoC test task, .class files are almost always stored uncompressed
   in a JAR. Full deflate can be added later. */
static CFResult extract_entry(const uint8_t *jar, uint32_t jar_len,
                              uint32_t local_offset,
                              uint32_t compressed_size,
                              uint32_t uncompressed_size,
                              uint16_t method,
                              uint8_t **out_data, uint32_t *out_len) 
{
    if (local_offset + 30 > jar_len) return CF_ERR_TRUNCATED;
    const uint8_t *lhdr = jar + local_offset;
    if (zip_u32le(lhdr) != ZIP_LOCAL_SIG) return CF_ERR_TRUNCATED;

    uint16_t fname_len = zip_u16le(lhdr + 26);
    uint16_t extra_len = zip_u16le(lhdr + 28);
    
    /* ─── FIX: Use 64-bit math to prevent overflow wrap-around ─── */
    uint64_t safe_off = (uint64_t)local_offset + 30 + fname_len + extra_len;
    if (safe_off + compressed_size > jar_len) return CF_ERR_TRUNCATED;
    
    uint32_t data_off = (uint32_t)safe_off;
    
    if (method == 0) {
        *out_data = (uint8_t *)malloc(uncompressed_size);
        if (!*out_data) return CF_ERR_IO;
        memcpy(*out_data, jar + data_off, uncompressed_size);
        *out_len = uncompressed_size; 
        return CF_OK;
    }
    fprintf(stderr, "[jar] compression method %d not supported\n", method);
    return CF_ERR_UNSUPPORTED;
}

CFResult jar_load(const char *jar_path, ClassRegistry *reg) {
    /* read entire JAR into memory */
    FILE *fp = fopen(jar_path, "rb");
    if (!fp) return CF_ERR_IO;
    
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { 
        fclose(fp); 
        return CF_ERR_IO; 
    }
    
    rewind(fp);
    
    uint8_t *jar = (uint8_t *)malloc((size_t)sz);
    if (!jar) { fclose(fp); return CF_ERR_IO; }
    
    if ((long)fread(jar, 1, (size_t)sz, fp) != sz) { 
        free(jar); 
        fclose(fp); 
        return CF_ERR_IO; 
    }
    fclose(fp);

    uint32_t jar_len = (uint32_t)sz;

    /* find EOCD */
    uint32_t cd_offset; uint16_t cd_entries;
    if (!find_eocd(jar, jar_len, &cd_offset, &cd_entries)) {
        free(jar); return CF_ERR_TRUNCATED;
    }

    /* walk central directory */
    uint32_t pos = cd_offset;
    CFResult result = CF_OK;

    for (uint16_t i = 0; i < cd_entries; i++) {
        if (pos + 46 > jar_len) { result = CF_ERR_TRUNCATED; break; }
        if (zip_u32le(jar + pos) != ZIP_CENTRAL_SIG) { result = CF_ERR_TRUNCATED; break; }

        uint16_t method      = zip_u16le(jar + pos + 10);
        uint32_t comp_size   = zip_u32le(jar + pos + 20);
        uint32_t uncomp_size = zip_u32le(jar + pos + 24);
        uint16_t fname_len   = zip_u16le(jar + pos + 28);
        uint16_t extra_len   = zip_u16le(jar + pos + 30);
        uint16_t comment_len = zip_u16le(jar + pos + 32);
        uint32_t local_off   = zip_u32le(jar + pos + 42);

        /* entry name */
        char *fname = (char *)malloc(fname_len + 1);
        if (!fname) { result = CF_ERR_IO; break; }
        memcpy(fname, jar + pos + 46, fname_len);
        fname[fname_len] = '\0';

        pos += 46 + fname_len + extra_len + comment_len;

        /* only process .class files */
        size_t nlen = strlen(fname);
        if (nlen > 6 && strcmp(fname + nlen - 6, ".class") == 0) {
            uint8_t *class_data = NULL;
            uint32_t class_len  = 0;
            CFResult er = extract_entry(jar, jar_len, local_off,
                                        comp_size, uncomp_size,
                                        method, &class_data, &class_len);
            if (er == CF_OK) {
                ClassFile *cf = NULL;
                er = classfile_parse(class_data, class_len, &cf);
                free(class_data);
                if (er == CF_OK) {
                    er = registry_add(reg, cf);
                    if (er != CF_OK) {
                        classfile_free(cf);
                        fprintf(stderr, "[jar] registry full, skipping %s\n", fname);
                    } else {
                        printf("[jar] loaded %s\n", fname);
                    }
                } else {
                    fprintf(stderr, "[jar] failed to parse %s: %s\n",
                            fname, cf_result_str(er));
                }
            } else {
                fprintf(stderr, "[jar] failed to extract %s: %s\n",
                        fname, cf_result_str(er));
            }
        }
        free(fname);
    }

    free(jar);
    return result;
}

/* ════════════════════════════════════════════════════════════
 *  Error string
 * ════════════════════════════════════════════════════════════ */

const char *cf_result_str(CFResult r) {
    switch (r) {
    case CF_OK:               return "OK";
    case CF_ERR_IO:           return "IO_ERROR";
    case CF_ERR_MAGIC:        return "BAD_MAGIC";
    case CF_ERR_TRUNCATED:    return "TRUNCATED";
    case CF_ERR_UNSUPPORTED:  return "UNSUPPORTED";
    case CF_ERR_NOT_FOUND:    return "NOT_FOUND";
    case CF_ERR_REGISTRY_FULL:return "REGISTRY_FULL";
    default:                  return "UNKNOWN";
    }
}
