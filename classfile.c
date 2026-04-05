/*
 * classfile.c — real .class file parser  (fixes issues #4, #5)
 *
 * Parses JVMS §4 binary format:
 *   magic, version, constant pool, access flags, this/super,
 *   interfaces (skipped), fields, methods with Code attribute.
 *
 * All parsed data lives in the KVMClass already in vm->classes[].
 * No dynamic allocation except strdup for name strings and code bytes.
 */

#include "jvm.h"

/* ── byte-order read helpers ─────────────────────────────── */
static uint8_t r8(const uint8_t *b, uint32_t *p)
{
    return b[(*p)++];
}
static uint16_t r16(const uint8_t *b, uint32_t *p)
{
    uint16_t v = (uint16_t)(((uint16_t)b[*p] << 8) | b[*p + 1]);
    *p += 2;
    return v;
}
static uint32_t r32(const uint8_t *b, uint32_t *p)
{
    uint32_t v = ((uint32_t)b[*p]     << 24) | ((uint32_t)b[*p + 1] << 16) |
                 ((uint32_t)b[*p + 2] <<  8) |  (uint32_t)b[*p + 3];
    *p += 4;
    return v;
}

/* ── read entire file into malloc buffer ─────────────────── */
static uint8_t *slurp(const char *path, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "kvm: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (uint32_t)sz;
    return buf;
}

/* ── look up a UTF8 entry in the constant pool ───────────── */
static const char *cp_utf8(KVMClass *cls, uint16_t idx)
{
    if (idx == 0 || idx >= cls->cp_count) return NULL;
    if (cls->cp[idx].tag != CP_UTF8)      return NULL;
    return cls->cp[idx].info.utf8.bytes;
}

/* ── parse raw class bytes into KVMClass ─────────────────── */
static JVMResult parse_class(const uint8_t *buf, uint32_t len, KVMClass *cls)
{
    uint32_t p = 0;

    if (len < 10) return JVM_ERR_CLASSFILE_INVALID;

    /* magic */
    if (r32(buf, &p) != 0xCAFEBABE) {
        fprintf(stderr, "kvm: not a .class file\n");
        return JVM_ERR_CLASSFILE_INVALID;
    }
    r16(buf, &p); /* minor version */
    r16(buf, &p); /* major version */

    /* ── constant pool ── */
    uint16_t cp_count = r16(buf, &p);
    if (cp_count >= MAX_CP_ENTRIES) {
        fprintf(stderr, "kvm: constant pool too large (%u)\n", cp_count);
        return JVM_ERR_CLASSFILE_INVALID;
    }
    cls->cp_count = cp_count;
    memset(cls->cp, 0, sizeof(cls->cp));

    for (uint16_t i = 1; i < cp_count; i++) {
        if (p >= len) return JVM_ERR_CLASSFILE_INVALID;
        uint8_t tag = r8(buf, &p);
        cls->cp[i].tag = tag;

        switch (tag) {
        case CP_UTF8: {
            uint16_t slen = r16(buf, &p);
            if (p + slen > len) return JVM_ERR_CLASSFILE_INVALID;
            char *s = (char *)malloc((size_t)slen + 1);
            if (!s) return JVM_ERR_OUT_OF_MEMORY;
            memcpy(s, buf + p, slen);
            s[slen] = '\0';
            p += slen;
            cls->cp[i].info.utf8.bytes  = s;
            cls->cp[i].info.utf8.length = slen;
            break;
        }
        case CP_INTEGER:
            cls->cp[i].info.integer_val = (int32_t)r32(buf, &p);
            break;
        case CP_FLOAT:
            r32(buf, &p); /* skip */
            break;
        case CP_CLASS:
            cls->cp[i].info.class_index  = r16(buf, &p);
            break;
        case CP_STRING:
            cls->cp[i].info.string_index = r16(buf, &p);
            break;
        case CP_FIELDREF:
        case CP_METHODREF:
        case CP_IFACE_MREF:
            cls->cp[i].info.ref.class_index = r16(buf, &p);
            cls->cp[i].info.ref.nat_index   = r16(buf, &p);
            break;
        case CP_NAME_AND_TYPE:
            cls->cp[i].info.nat.name_index = r16(buf, &p);
            cls->cp[i].info.nat.desc_index = r16(buf, &p);
            break;
        case 5: case 6:   /* Long / Double occupy two CP slots */
            r32(buf, &p); r32(buf, &p);
            i++;
            break;
        default:
            fprintf(stderr, "kvm: unknown CP tag %u at index %u\n", tag, i);
            return JVM_ERR_CLASSFILE_INVALID;
        }
    }

    /* access_flags, this_class, super_class */
    r16(buf, &p); /* access_flags */
    uint16_t this_class  = r16(buf, &p);
    r16(buf, &p); /* super_class — ignored for now */

    /* class name */
    if (this_class && this_class < cp_count &&
        cls->cp[this_class].tag == CP_CLASS) {
        const char *raw = cp_utf8(cls, cls->cp[this_class].info.class_index);
        if (raw) cls->name = strdup(raw);
    }

    /* interfaces (skip) */
    uint16_t iface_count = r16(buf, &p);
    p += (uint32_t)iface_count * 2;

    /* ── fields ── */
    uint16_t field_count = r16(buf, &p);
    cls->field_count = 0;
    for (uint16_t fi = 0; fi < field_count; fi++) {
        uint16_t acc = r16(buf, &p);
        uint16_t ni  = r16(buf, &p);
        uint16_t di  = r16(buf, &p);
        uint16_t ac  = r16(buf, &p);
        if (cls->field_count < MAX_FIELDS) {
            FieldInfo *fd = &cls->fields[cls->field_count++];
            const char *fn  = cp_utf8(cls, ni);
            const char *fds = cp_utf8(cls, di);
            fd->name        = strdup(fn  ? fn  : "?");
            fd->descriptor  = strdup(fds ? fds : "?");
            fd->access_flags = acc;
            fd->value        = 0;
        }
        /* skip field attributes */
        for (uint16_t ai = 0; ai < ac; ai++) {
            r16(buf, &p);
            uint32_t alen = r32(buf, &p);
            p += alen;
        }
    }

    /* ── methods ── */
    uint16_t method_count = r16(buf, &p);
    cls->method_count = 0;
    for (uint16_t mi = 0; mi < method_count && cls->method_count < MAX_METHODS; mi++) {
        uint16_t acc = r16(buf, &p);
        uint16_t ni  = r16(buf, &p);
        uint16_t di  = r16(buf, &p);
        uint16_t ac  = r16(buf, &p);

        MethodInfo *m   = &cls->methods[cls->method_count++];
        const char *mn  = cp_utf8(cls, ni);
        const char *mds = cp_utf8(cls, di);
        m->name         = strdup(mn  ? mn  : "?");
        m->descriptor   = strdup(mds ? mds : "?");
        m->access_flags = acc;
        m->code         = NULL;
        m->code_len     = 0;
        m->max_stack    = MAX_STACK;
        m->max_locals   = MAX_LOCALS;

        for (uint16_t ai = 0; ai < ac; ai++) {
            uint16_t attr_ni = r16(buf, &p);
            uint32_t alen    = r32(buf, &p);
            uint32_t end_p   = p + alen;

            const char *aname = cp_utf8(cls, attr_ni);
            if (aname && strcmp(aname, "Code") == 0) {
                m->max_stack  = r16(buf, &p);
                m->max_locals = r16(buf, &p);
                uint32_t code_len = r32(buf, &p);
                m->code = (uint8_t *)malloc(code_len);
                if (!m->code) return JVM_ERR_OUT_OF_MEMORY;
                memcpy(m->code, buf + p, code_len);
                m->code_len  = code_len;
                m->code_owned = 1;
                /* exception table + inner attributes skipped via end_p */
            }
            p = end_p;
        }
    }

    cls->loaded = 1;
    return JVM_OK;
}

/* ── free all CP strings in a class (on parse error) ────── */
static void free_cp(KVMClass *cls)
{
    for (int i = 1; i < cls->cp_count; i++)
        if (cls->cp[i].tag == CP_UTF8 && cls->cp[i].info.utf8.bytes) {
            free(cls->cp[i].info.utf8.bytes);
            cls->cp[i].info.utf8.bytes = NULL;
        }
}

/* ── public: load a .class file into the VM ─────────────── */
JVMResult vm_load_class(VM *vm, const char *path, KVMClass **out_class)
{
    if (vm->class_count >= MAX_CLASSES) return JVM_ERR_OUT_OF_MEMORY;

    uint32_t len = 0;
    uint8_t *buf = slurp(path, &len);
    if (!buf) return JVM_ERR_CLASS_NOT_FOUND;

    KVMClass *cls = &vm->classes[vm->class_count];
    memset(cls, 0, sizeof(KVMClass));

    JVMResult r = parse_class(buf, len, cls);
    free(buf);

    if (r != JVM_OK) {
        free_cp(cls);
        if (cls->name) { free(cls->name); cls->name = NULL; }
        return r;
    }

    vm->class_count++;
    if (out_class) *out_class = cls;

    if (vm->verbose)
        printf("[classloader] '%s' loaded from '%s'  (%u methods, %u fields)\n",
               cls->name ? cls->name : "?", path,
               cls->method_count, cls->field_count);

    return JVM_OK;
}

/* ── find an already-loaded class by name ────────────────── */
KVMClass *vm_find_class(VM *vm, const char *name)
{
    for (int i = 0; i < vm->class_count; i++) {
        KVMClass *c = &vm->classes[i];
        if (c->loaded && c->name && strcmp(c->name, name) == 0)
            return c;
    }
    return NULL;
}
