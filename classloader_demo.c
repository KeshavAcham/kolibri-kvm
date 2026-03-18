#include "jvm.h"
#include "classfile.h"

/* ════════════════════════════════════════════════════════════
 *  Usage:
 *    ./kvm Hello.class           – run a single .class file
 *    ./kvm app.jar com/example/Main  – run Main from a JAR
 * ════════════════════════════════════════════════════════════ */

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <file.class>                  - run a .class file\n", prog);
    fprintf(stderr, "  %s <file.jar> <ClassName>        - run a class from a JAR\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    ClassRegistry reg;
    registry_init(&reg);

    const char *path       = argv[1];
    const char *class_name = NULL;  /* for JAR mode */

    /* Determine if it's a JAR or .class file */
    size_t plen = strlen(path);
    int is_jar = (plen > 4 && strcmp(path + plen - 4, ".jar") == 0);

    if (is_jar) {
        if (argc < 3) {
            fprintf(stderr, "Error: JAR mode requires a class name argument.\n");
            usage(argv[0]); return 1;
        }
        class_name = argv[2];
        printf("[kvm] Loading JAR: %s\n", path);
        CFResult r = jar_load(path, &reg);
        if (r != CF_OK) {
            fprintf(stderr, "[kvm] Failed to load JAR: %s\n", cf_result_str(r));
            return 1;
        }
    } else {
        /* Single .class file */
        printf("[kvm] Loading class file: %s\n", path);
        ClassFile *cf = NULL;
        CFResult r = classfile_load(path, &cf);
        if (r != CF_OK) {
            fprintf(stderr, "[kvm] Failed to load class: %s\n", cf_result_str(r));
            return 1;
        }
        registry_add(&reg, cf);
        class_name = cf->class_name;
    }

    /* Find the class */
    ClassFile *cf = registry_find(&reg, class_name);
    if (!cf) {
        fprintf(stderr, "[kvm] Class not found: %s\n", class_name);
        return 1;
    }

    printf("[kvm] Class: %s  (%d methods)\n", cf->class_name, cf->methods_count);

    /* Print all methods found */
    printf("[kvm] Methods:\n");
    for (int i = 0; i < cf->methods_count; i++) {
        printf("  [%d] %s %s\n", i,
               cf->methods[i].name,
               cf->methods[i].descriptor);
    }

    /* Try to find and run 'main' or any method with bytecode */
    MethodInfo *m = classfile_find_method(cf, "main", NULL);
    if (!m) {
        /* Fall back to first method that has bytecode */
        for (int i = 0; i < cf->methods_count; i++) {
            if (cf->methods[i].code) { m = &cf->methods[i]; break; }
        }
    }

    if (!m || !m->code) {
        fprintf(stderr, "[kvm] No executable method found.\n");
        return 1;
    }

    printf("[kvm] Running method: %s%s\n", m->name, m->descriptor);

    VM vm;
    vm_init(&vm, 1 /* verbose */);

    int32_t ret = 0;
    JVMResult jr = vm_exec(&vm, m->code, m->code_length, &ret);

    printf("[kvm] Result: %s", jvm_result_str(jr));
    if (jr == JVM_RETURN_INT) printf("  return value = %d", ret);
    printf("\n");

    /* Free all classes */
    for (int i = 0; i < reg.count; i++)
        classfile_free(reg.classes[i]);

    return (jr == JVM_OK || jr == JVM_RETURN_INT || jr == JVM_RETURN_VOID) ? 0 : 1;
}
