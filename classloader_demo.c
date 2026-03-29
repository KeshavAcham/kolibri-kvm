/*
 * classloader_demo.c — loads a real .class file and executes a method
 * through vm_exec_full(), wiring the parsed CP and exception table.
 *
 * Usage:
 *   ./kvm Hello.class          — runs first method found
 *   ./kvm Hello.class run      — runs method named "run"
 *
 * Phase 1 scope: single-class, single-method execution.
 * Multi-class dispatch and JAR support are Phase 2 deliverables.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jvm.h"

static uint16_t ru2(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t ru4(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

typedef struct {
    uint8_t  tag;
    uint16_t ref1;
    uint16_t ref2;
    int32_t  ival;
    float    fval;
    char    *utf8;   /* heap-allocated, must be freed */
} RawCP;

typedef struct {
    const uint8_t *code;
    uint32_t       code_len;
    uint16_t       max_stack;
    uint16_t       max_locals;
    ExceptionEntry *exc;
    uint16_t        exc_len;
} CodeAttr;

static int parse_code_attr(const uint8_t *buf, uint32_t len, CodeAttr *out)
{
    if (len < 8) return 0;
    out->max_stack  = ru2(buf);
    out->max_locals = ru2(buf + 2);
    out->code_len   = ru4(buf + 4);
    if (8 + out->code_len > len) return 0;
    out->code = buf + 8;

    uint32_t off = 8 + out->code_len;
    if (off + 2 > len) return 0;
    uint16_t etbl = ru2(buf + off); off += 2;
    out->exc_len = etbl;
    out->exc = NULL;
    if (etbl > 0) {
        if (off + (uint32_t)etbl * 8 > len) return 0;
        out->exc = malloc(sizeof(ExceptionEntry) * etbl);
        if (!out->exc) return 0;
        for (uint16_t i = 0; i < etbl; i++) {
            out->exc[i].start_pc   = ru2(buf + off); off += 2;
            out->exc[i].end_pc     = ru2(buf + off); off += 2;
            out->exc[i].handler_pc = ru2(buf + off); off += 2;
            out->exc[i].catch_type = ru2(buf + off); off += 2;
        }
    }
    return 1;
}

static int load_class(const char *path, const char *want_method,
                      CPEntry **cp_out, uint16_t *cp_len_out,
                      CodeAttr *code_out, uint8_t **buf_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "[kvm] Cannot open: %s\n", path); return 0; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return 0; }
    rewind(fp);

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return 0; }
    if ((long)fread(buf, 1, (size_t)sz, fp) != sz) {
        free(buf); fclose(fp); return 0;
    }
    fclose(fp);
    *buf_out = buf;

    uint32_t off = 0;
    uint32_t len = (uint32_t)sz;

#define NEED(n) do { if (off + (n) > len) { fprintf(stderr,"[kvm] Truncated at off=%u\n",off); goto fail; } } while(0)

    NEED(8);
    if (ru4(buf) != 0xCAFEBABE) { fprintf(stderr,"[kvm] Bad magic\n"); goto fail; }
    off = 8;

    /* constant pool */
    NEED(2);
    uint16_t cp_count = ru2(buf + off); off += 2;
    RawCP *rcp = calloc(cp_count, sizeof(RawCP));
    if (!rcp) goto fail;

    for (uint16_t i = 1; i < cp_count; i++) {
        NEED(1);
        uint8_t tag = buf[off++];
        rcp[i].tag = tag;
        switch (tag) {
        case 1: {
            NEED(2);
            uint16_t ulen = ru2(buf + off); off += 2;
            NEED(ulen);
            rcp[i].utf8 = malloc(ulen + 1);
            if (!rcp[i].utf8) goto fail_rcp;
            memcpy(rcp[i].utf8, buf + off, ulen);
            rcp[i].utf8[ulen] = '\0';
            off += ulen;
            break;
        }
        case 3: NEED(4); rcp[i].ival = (int32_t)ru4(buf+off); off+=4; break;
        case 4: { NEED(4); uint32_t b=ru4(buf+off); memcpy(&rcp[i].fval,&b,4); off+=4; break; }
        case 5: case 6: NEED(8); off+=8; i++; break;
        case 7: case 8: NEED(2); rcp[i].ref1=ru2(buf+off); off+=2; break;
        case 9: case 10: case 11: case 12:
            NEED(4); rcp[i].ref1=ru2(buf+off); off+=2; rcp[i].ref2=ru2(buf+off); off+=2; break;
        default:
            fprintf(stderr,"[kvm] Unknown CP tag %u at #%u\n", tag, i);
            goto fail_rcp;
        }
    }

    /* Build CPEntry[] */
    CPEntry *cp = calloc(cp_count, sizeof(CPEntry));
    if (!cp) goto fail_rcp;
    for (uint16_t i = 1; i < cp_count; i++) {
        switch (rcp[i].tag) {
        case 3: cp[i].tag=CP_INTEGER; cp[i].ival=rcp[i].ival; break;
        case 4: cp[i].tag=CP_FLOAT;   cp[i].fval=rcp[i].fval; break;
        case 8:
            cp[i].tag=CP_STRING;
            if (rcp[i].ref1 < cp_count && rcp[rcp[i].ref1].utf8)
                cp[i].sval = rcp[rcp[i].ref1].utf8;
            break;
        default: cp[i].tag=CP_EMPTY; break;
        }
    }
    *cp_out     = cp;
    *cp_len_out = cp_count;

    /* access_flags(2) + this_class(2) + super_class(2) */
    NEED(6); off += 6;
    /* interfaces */
    NEED(2); uint16_t iface_count = ru2(buf+off); off+=2;
    NEED((uint32_t)iface_count*2); off += (uint32_t)iface_count*2;
    /* fields */
    NEED(2); uint16_t field_count = ru2(buf+off); off+=2;
    for (uint16_t i = 0; i < field_count; i++) {
        NEED(6); off+=6;
        NEED(2); uint16_t ac = ru2(buf+off); off+=2;
        for (uint16_t a = 0; a < ac; a++) {
            NEED(6); off+=2;
            uint32_t alen = ru4(buf+off); off+=4;
            NEED(alen); off+=alen;
        }
    }
    /* methods */
    NEED(2); uint16_t method_count = ru2(buf+off); off+=2;
    printf("[kvm] %s — %u method(s)\n", path, method_count);

    int found = 0;
    for (uint16_t i = 0; i < method_count && !found; i++) {
        NEED(8); off+=2; /* access_flags */
        uint16_t name_idx = ru2(buf+off); off+=2;
        off+=2; /* descriptor_index */
        uint16_t attr_count = ru2(buf+off); off+=2;

        const char *mname = (name_idx<cp_count && rcp[name_idx].utf8)
                            ? rcp[name_idx].utf8 : "?";
        printf("[kvm]   [%u] %s\n", i, mname);

        int want = (want_method==NULL && i==0) ||
                   (want_method!=NULL && strcmp(mname,want_method)==0);

        for (uint16_t a = 0; a < attr_count; a++) {
            NEED(6); off+=2; /* attr_name unused here for simplicity */
            /* re-read attr name properly */
            uint16_t aname_idx = ru2(buf+off-2);
            uint32_t alen = ru4(buf+off); off+=4;
            NEED(alen);
            const char *aname = (aname_idx<cp_count && rcp[aname_idx].utf8)
                                ? rcp[aname_idx].utf8 : "";
            if (want && strcmp(aname,"Code")==0 && !found) {
                if (!parse_code_attr(buf+off, alen, code_out)) {
                    fprintf(stderr,"[kvm] Failed to parse Code attr\n");
                    goto fail_cp;
                }
                printf("[kvm] Running method: %s\n", mname);
                found = 1;
            }
            off += alen;
        }
    }

    /* Free raw CP (keep utf8 strings referenced by CP_STRING entries) */
    for (uint16_t i = 1; i < cp_count; i++) {
        if (rcp[i].tag != 1) continue;
        int keep = 0;
        for (uint16_t j = 1; j < cp_count; j++)
            if (rcp[j].tag == 8 && rcp[j].ref1 == i) { keep=1; break; }
        if (!keep) { free(rcp[i].utf8); rcp[i].utf8=NULL; }
    }
    free(rcp);

    if (!found) {
        fprintf(stderr,"[kvm] Method '%s' not found\n",
                want_method ? want_method : "(first)");
        free(cp); return 0;
    }
    return 1;

fail_cp: free(cp);
fail_rcp:
    for (uint16_t i=1; i<cp_count; i++) free(rcp[i].utf8);
    free(rcp);
fail:
    return 0;
#undef NEED
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,"Usage: %s <file.class> [method_name]\n", argv[0]);
        return 1;
    }
    const char *class_path  = argv[1];
    const char *method_name = (argc >= 3) ? argv[2] : NULL;

    CPEntry  *cp     = NULL;
    uint16_t  cp_len = 0;
    CodeAttr  code;  memset(&code, 0, sizeof(code));
    uint8_t  *buf    = NULL;

    if (!load_class(class_path, method_name, &cp, &cp_len, &code, &buf)) {
        fprintf(stderr,"[kvm] Load failed\n"); return 1;
    }

    printf("[kvm] Bytecode: %u bytes  max_stack=%u  max_locals=%u\n",
           code.code_len, code.max_stack, code.max_locals);

    VM vm; vm_init(&vm, 1);
    int32_t ret_val = 0;
    JVMResult r = vm_exec_full(&vm, code.code, code.code_len,
                               cp, cp_len, code.exc, code.exc_len, &ret_val);

    printf("[kvm] Result: %s", jvm_result_str(r));
    if (r == JVM_RETURN_INT)   printf("  return = %d", ret_val);
    if (r == JVM_RETURN_FLOAT) { float f; memcpy(&f,&ret_val,4); printf("  return = %f", f); }
    if (r == JVM_RETURN_REF)   printf("  return ref = %d", ret_val);
    printf("\n");

    free(cp);
    if (code.exc) free(code.exc);
    free(buf);
    return (r==JVM_RETURN_INT||r==JVM_RETURN_VOID||
            r==JVM_RETURN_FLOAT||r==JVM_RETURN_REF) ? 0 : 1;
}
