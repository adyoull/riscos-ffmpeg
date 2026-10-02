/*
 * eabiprobe (tools/eabiprobe) against a fake RISC OS: modules in memory
 * (ARMEABISupport, SharedUnixLibrary, PThreadTicker; one of them twice),
 * module files in a fake System:Modules and a fake !Iris, a fake
 * *ARMEABISupport_Info, the Task Manager's list and directory listings.
 *   - Raik's suspected machine: 1.02 loaded and in System:Modules, 1.05
 *     inside !Iris (its !Run asks for 1.05): the verdict names Iris's copy;
 *   - everything 1.05: OK;
 *   - ARMEABISupport not loaded: said so;
 *   - the report: versions, ROM/RMA, instances, SWI names, the Info output
 *     with its count, the tasks (a name at an odd address, ended by a
 *     control character), Iris's !Run lines and its modules.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "kernel.h"

int probe_main(int argc, char **argv);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static _kernel_oserror err = { 1, "fake error" };
#define T "/tmp/eabiprobe_test"

/* ---- modules: [length word][header][strings] ---- */

typedef struct { uint8_t *mem; uint32_t base; } fakemod_t;

static uint32_t make_module(const char *title, const char *help, uint32_t chunk, const char *const *swis, uint8_t *out)
{
    uint32_t h[13] = { 0 }, o = 0x34;
    uint8_t body[1024];
    memset(body, 0, sizeof body);
    h[4] = o; strcpy((char *)body + o, title); o += (uint32_t)strlen(title) + 1;
    h[5] = o; strcpy((char *)body + o, help); o += (uint32_t)strlen(help) + 1;
    h[7] = chunk;
    if (swis) {
        h[9] = o;
        for (int i = 0; swis[i]; i++) { strcpy((char *)body + o, swis[i]); o += (uint32_t)strlen(swis[i]) + 1; }
        body[o++] = 0;
    }
    o = (o + 3) & ~3u;
    memcpy(body, h, sizeof h);
    if (out) memcpy(out, body, o);
    return o;
}

static const char *const eabi_swis[] = { "ARMEABISupport", "MemoryOp", "AbortOp", "StackOp", "Cleanup", "MMapOp", "ShmOp", NULL };
static uint8_t *loaded[8];
static int nloaded, eabi_loaded;

static void load(const char *title, const char *help, uint32_t chunk, const char *const *swis)
{
    uint32_t len = make_module(title, help, chunk, swis, NULL);
    uint8_t *m = aligned_alloc(4, len + 8);
    memset(m, 0, len + 8);
    *(uint32_t *)m = len + 4;
    make_module(title, help, chunk, swis, m + 4);
    loaded[nloaded++] = m + 4;
}

static void unload_all(void)
{
    for (int i = 0; i < nloaded; i++) free(loaded[i] - 4);
    nloaded = 0;
}

static void write_module_file(const char *path, const char *title, const char *help)
{
    uint8_t buf[1024];
    uint32_t len = make_module(title, help, !strcmp(title, "ARMEABISupport") ? 0x59D00 : 0,
                               !strcmp(title, "ARMEABISupport") ? eabi_swis : NULL, buf);
    FILE *f = fopen(path, "wb");
    fwrite(buf, 1, len, f);
    fclose(f);
}

/* ---- names: RISC OS -> host ---- */

static void host_name(const char *in, char *out, size_t n)
{
    char tmp[512];
    if (!strncmp(in, "System:Modules.", 15)) snprintf(tmp, sizeof tmp, T "/sys/%s", in + 15);
    else if (!strncmp(in, "IRIS", 4)) {
        snprintf(tmp, sizeof tmp, T "/iris%s", in + 4);
        for (char *p = tmp + strlen(T "/iris"); *p; p++) if (*p == '.') *p = '/';
    } else snprintf(tmp, sizeof tmp, "%s", in);
    snprintf(out, n, "%s", tmp);
    {
        struct stat st;
        char ffa[520];
        snprintf(ffa, sizeof ffa, "%s,ffa", tmp);
        if (stat(tmp, &st) && !stat(ffa, &st)) snprintf(out, n, "%s", ffa);
    }
}

FILE *probe_fopen(const char *name, const char *mode)
{
    char h[520];
    host_name(name, h, sizeof h);
    return fopen(h, mode);
}

void probe_copy(uintptr_t src, void *dst, size_t n) { memcpy(dst, (const void *)src, n); }

/* ---- SWIs ---- */

static char task_names[64];
static int info_runs;

_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r)
{
    switch (n) {
    case 0x1E:                                         /* OS_Module */
        if (r->r[0] == 18) {
            const char *want = (const char *)(uintptr_t)r->r[1];
            for (int i = 0; i < nloaded; i++) {
                uint8_t *m = loaded[i];
                if (!strcmp((char *)m + *(uint32_t *)(m + 0x10), want)) {
                    r->r[3] = (int)(uintptr_t)m;
                    r->r[4] = 0x1234 + i;
                    return NULL;
                }
            }
            return &err;
        }
        if (r->r[0] == 12) {
            if (r->r[1] >= nloaded) return &err;
            r->r[3] = (int)(uintptr_t)loaded[r->r[1]];
            r->r[1]++;
            r->r[2] = 0;
            return NULL;
        }
        break;
    case 0x29:                                         /* OS_FSControl 37 */
        CHECK(r->r[0] == 37, "FSControl %d", (int)r->r[0]);
        snprintf((char *)(uintptr_t)r->r[2], (size_t)r->r[5], "SDFS::Pi.$.%s", (const char *)(uintptr_t)r->r[1]);
        return NULL;
    case 0x05: {                                       /* OS_CLI: *ARMEABISupport_Info { > file } */
        const char *c = (const char *)(uintptr_t)r->r[0];
        char file[256], h[520];
        FILE *f;
        CHECK(!strncmp(c, "ARMEABISupport_Info { > ", 24), "command '%s'", c);
        if (!eabi_loaded) return &err;
        info_runs++;
        snprintf(file, sizeof file, "%s", c + 24);
        *strchr(file, ' ') = 0;
        host_name(file, h, sizeof h);
        f = fopen(h, "w");
        fprintf(f, "App 3E1A8000\n  Stack: main 0x10000 -> 0x20000\nApp 3F004000\nGlobal allocators:\nmmaps:\nSHM:\n");
        fclose(f);
        return NULL;
    }
    case 0x42681: {                                    /* TaskManager_EnumerateTasks */
        uint32_t *b = (uint32_t *)(uintptr_t)r->r[1];
        static const char *const names[] = { "Filer", "Reel\x0D", "eabiprobe" };
        int k = r->r[0];
        if (k >= 3) { r->r[0] = -1; return NULL; }
        strcpy(task_names + 1 + 20 * k, names[k]);      /* (odd addresses) */
        b[0] = 0x100 + k; b[1] = (uint32_t)(uintptr_t)(task_names + 1 + 20 * k);
        b[2] = (uint32_t)(k == 1 ? 40960 : 640) << 10; b[3] = k == 0;
        r->r[1] = (int)(uintptr_t)(b + 4);
        r->r[0] = k + 1 < 3 ? k + 1 : -1;
        return NULL;
    }
    case 0x0C: {                                       /* OS_GBPB 10 */
        char h[520];
        DIR *d;
        struct dirent *e;
        uint8_t *out = (uint8_t *)(uintptr_t)r->r[2];
        int i = 0, got = 0;
        CHECK(r->r[0] == 10, "GBPB %d", (int)r->r[0]);
        host_name((const char *)(uintptr_t)r->r[1], h, sizeof h);
        if (!(d = opendir(h))) return &err;
        while ((e = readdir(d)) != NULL) {
            char full[1100], name[256];
            struct stat st;
            uint32_t rec[5] = { 0 };
            if (e->d_name[0] == '.') continue;
            if (i++ < r->r[4]) continue;
            snprintf(full, sizeof full, "%s/%s", h, e->d_name);
            stat(full, &st);
            snprintf(name, sizeof name, "%s", e->d_name);
            if (strstr(name, ",ffa")) { *strstr(name, ",ffa") = 0; rec[0] = 0xFFFFFA00u; }
            rec[4] = S_ISDIR(st.st_mode) ? (name[0] == '!' ? 3 : 2) : 1;
            memcpy(out, rec, 20);
            strcpy((char *)out + 20, name);
            out += (20 + strlen(name) + 1 + 3) & ~3u;
            got++;
        }
        closedir(d);
        r->r[3] = got;
        r->r[4] = -1;
        return NULL;
    }
    }
    CHECK(0, "unexpected SWI &%X", n);
    return &err;
}

static char *run(int *ret, const char *label)
{
    static char buf[16384];
    char *argv[] = { "eabiprobe", "-o", T "/out", "-i", T "/info", "-l", (char *)label, NULL };
    FILE *f;
    size_t got;
    remove(T "/out");
    info_runs = 0;
    *ret = probe_main(7, argv);
    f = fopen(T "/out", "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    return buf;
}

static void setup(const char *loaded_eabi, const char *sys_eabi, const char *iris_eabi)
{
    FILE *f;
    unload_all();
    if (system("rm -rf " T " && mkdir -p " T "/sys " T "/iris/Resources/Modules")) {}
    eabi_loaded = loaded_eabi != NULL;
    if (loaded_eabi) load("ARMEABISupport", loaded_eabi, 0x59D00, eabi_swis);
    load("SharedUnixLibrary", "SharedUnixLibrary\t1.16 (15 Mar 2021)", 0x55C80, NULL);
    load("PThreadTicker", "PThreadTicker\t0.03 (01 Oct 2026)", 0, NULL);
    load("PThreadTicker", "PThreadTicker\t0.03 (01 Oct 2026)", 0, NULL);  /* (twice: "more than one") */
    write_module_file(T "/sys/ARMEABISupport,ffa", "ARMEABISupport", sys_eabi);
    write_module_file(T "/sys/SharedULib,ffa", "SharedUnixLibrary", "SharedUnixLibrary\t1.16 (15 Mar 2021)");
    write_module_file(T "/iris/Resources/Modules/ARMEABISupport,ffa", "ARMEABISupport", iris_eabi);
    write_module_file(T "/iris/Resources/Modules/SharedULib,ffa", "SharedUnixLibrary", "SharedUnixLibrary\t1.16 (15 Mar 2021)");
    f = fopen(T "/iris/!Run", "w");
    fprintf(f, "Set Iris$Dir <Obey$Dir>\nRMEnsure ARMEABISupport 1.05 RMLoad <Iris$Dir>.Resources.Modules.ARMEABISupport\n"
               "WimpSlot -min 2048K\nRun <Iris$Dir>.!RunImage %%*0\n");
    fclose(f);
    setenv("Iris$Dir", "IRIS", 1);
}

int main(void)
{
    int ret;
    char *o;

    /* Raik's machine, as we think: 1.02 loaded and in !System, 1.05 in !Iris */
    setup("ARMEABISupport\t1.02 (11 May 2021)", "ARMEABISupport\t1.02 (11 May 2021)", "ARMEABISupport\t1.05 (07 Jan 2023)");
    o = run(&ret, "after Reel");
    printf("%s", o);
    CHECK(strstr(o, "- after Reel"), "label");
    CHECK(strstr(o, "ARMEABISupport     ARMEABISupport 1.02 (11 May 2021)"), "loaded version");
    CHECK(strstr(o, "(RMA), workspace &00001234"), "where");
    CHECK(strstr(o, "SWI chunk &59D00: (ARMEABISupport_) MemoryOp AbortOp StackOp Cleanup MMapOp ShmOp"), "SWIs");
    CHECK(strstr(o, "more than one instance"), "PThreadTicker twice");
    CHECK(strstr(o, "VFPSupport         not loaded"), "not loaded");
    CHECK(strstr(o, "SDFS::Pi.$.System:Modules.ARMEABISupport\n      ARMEABISupport 1.02"), "System copy");
    CHECK(strstr(o, "System:Modules.PThrTicker: none"), "missing System copy");
    CHECK(info_runs == 1 && strstr(o, "  App 3E1A8000\n") && strstr(o, "(2 programs)"), "Info (%d)", info_runs);
    CHECK(strstr(o, "  Reel                              40960K\n") && strstr(o, "  Filer") && strstr(o, "(module)"), "tasks");
    CHECK(strstr(o, "RMEnsure ARMEABISupport 1.05 RMLoad <Iris$Dir>.Resources.Modules.ARMEABISupport") &&
          !strstr(o, "WimpSlot"), "Iris's !Run lines");
    CHECK(strstr(o, "IRIS.Resources.Modules.ARMEABISupport\n      ARMEABISupport 1.05"), "Iris's modules");
    CHECK(ret == 2 && strstr(o, "Result: a newer ARMEABISupport (1.05) is in\n  SDFS::Pi.$.IRIS.Resources.Modules.ARMEABISupport") &&
          strstr(o, "than the one loaded (1.02)") && strstr(o, "There are still clients"), "verdict (%d)", ret);

    /* everything 1.05 */
    setup("ARMEABISupport\t1.05 (07 Jan 2023)", "ARMEABISupport\t1.05 (07 Jan 2023)", "ARMEABISupport\t1.05 (07 Jan 2023)");
    o = run(&ret, "all 1.05");
    CHECK(ret == 0 && strstr(o, "Result: OK - no copy found is newer than the loaded ARMEABISupport (1.05)"), "all 1.05 (%d):\n%s", ret, o);

    /* !System newer than what's loaded (an old one loaded at boot) */
    setup("ARMEABISupport\t1.03 (15 Jun 2021)", "ARMEABISupport\t1.05 (07 Jan 2023)", "ARMEABISupport\t1.04 (21 Dec 2021)");
    o = run(&ret, "old loaded");
    CHECK(ret == 2 && strstr(o, "newer ARMEABISupport (1.05) is in\n  SDFS::Pi.$.System:Modules.ARMEABISupport"), "System newer (%d)", ret);

    /* not loaded */
    setup(NULL, "ARMEABISupport\t1.05 (07 Jan 2023)", "ARMEABISupport\t1.05 (07 Jan 2023)");
    o = run(&ret, "none");
    CHECK(strstr(o, "ARMEABISupport     not loaded") && strstr(o, "the command failed") && strstr(o, "isn't loaded"), "not loaded:\n%s", o);

    /* no Iris seen */
    setup("ARMEABISupport\t1.05 (07 Jan 2023)", "ARMEABISupport\t1.05 (07 Jan 2023)", "ARMEABISupport\t1.05 (07 Jan 2023)");
    unsetenv("Iris$Dir");
    o = run(&ret, "no Iris");
    CHECK(strstr(o, "Iris$Dir isn't set") && !strstr(o, "Modules inside it"), "no Iris");

    unload_all();
    printf(fails ? "eabiprobe_test: %d failures\n" : "eabiprobe_test: all passed\n", fails);
    return fails != 0;
}
