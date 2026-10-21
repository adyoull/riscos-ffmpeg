/* insprof: instructions executed per translated block, all of them, written
   at exit to the file named by the "out" argument (default insprof.txt) as
   "PC INSTRUCTIONS" lines, for summing per function against a symbol table. */
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct { uint64_t pc, n_insns, execs; } Block;
static GHashTable *blocks;
static GMutex lock;
static const char *out = "insprof.txt";

static void exec_cb(unsigned int cpu, void *udata)
{
    __atomic_fetch_add(&((Block *)udata)->execs, 1, __ATOMIC_RELAXED);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    size_t n = qemu_plugin_tb_n_insns(tb);
    Block *b;
    g_mutex_lock(&lock);
    uint64_t key = pc ^ ((uint64_t)n << 48);
    b = g_hash_table_lookup(blocks, &key);
    if (!b) {
        uint64_t *k = g_new(uint64_t, 1);
        *k = key;
        b = g_new0(Block, 1);
        b->pc = pc;
        b->n_insns = n;
        g_hash_table_insert(blocks, k, b);
    }
    g_mutex_unlock(&lock);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, exec_cb, QEMU_PLUGIN_CB_NO_REGS, b);
}

static void dump(gpointer k, gpointer v, gpointer f)
{
    Block *b = v;
    if (b->execs)
        fprintf(f, "%llx %llu\n", (unsigned long long)b->pc, (unsigned long long)(b->execs * b->n_insns));
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
    FILE *f = fopen(out, "w");
    if (!f)
        return;
    g_hash_table_foreach(blocks, dump, f);
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    for (int i = 0; i < argc; i++)
        if (!strncmp(argv[i], "out=", 4))
            out = g_strdup(argv[i] + 4);
    blocks = g_hash_table_new(g_int64_hash, g_int64_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
