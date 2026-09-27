/*
 * convert_cmds PROBE_OUTPUT SRC OUT [preset=N] [format=N] [size=N]
 *              [quality=N] [speed=N] [sound=0|1] [deint=0|1] [easy=0|1]
 *              [from=T] [to=T]
 * Prints the ffmpeg arguments the Convert window would use, from real
 * ffprobe output (the window's own query), so run.sh can run them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "convert.h"

/* the window's Wimp side isn't used here */
_kernel_oserror *_kernel_swi(int n, _kernel_swi_regs *in, _kernel_swi_regs *out) { static _kernel_oserror e; return &e; }
_kernel_oserror *fffront_start_task(const char *cmd) { return NULL; }
void fffront_play(const char *file) { }

int main(int argc, char **argv)
{
    static char probe[8192];
    char args[1200], err[200];
    ConvSettings s;
    ConvSource in;
    FILE *f = fopen(argv[1], "r");
    size_t n = f ? fread(probe, 1, sizeof(probe) - 1, f) : 0;
    if (f) fclose(f);
    probe[n] = 0;
    conv_parse_probe(&in, probe);
    memset(&s, 0, sizeof(s));
    conv_apply_preset(&s, 0);
    s.deinterlace = 1;
    for (int i = 4; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        int v = eq ? atoi(eq + 1) : 0;
        if (!eq) continue;
        if (!strncmp(argv[i], "preset=", 7)) conv_apply_preset(&s, v);
        else if (!strncmp(argv[i], "format=", 7)) s.format = v;
        else if (!strncmp(argv[i], "size=", 5)) s.size = v;
        else if (!strncmp(argv[i], "quality=", 8)) s.quality = v;
        else if (!strncmp(argv[i], "speed=", 6)) s.speed = v;
        else if (!strncmp(argv[i], "sound=", 6)) s.sound = v;
        else if (!strncmp(argv[i], "deint=", 6)) s.deinterlace = v;
        else if (!strncmp(argv[i], "easy=", 5)) s.easy = v;
        else if (!strncmp(argv[i], "from=", 5)) snprintf(s.from, sizeof(s.from), "%s", eq + 1);
        else if (!strncmp(argv[i], "to=", 3)) snprintf(s.to, sizeof(s.to), "%s", eq + 1);
    }
    if (conv_build_args(args, sizeof(args), &s, &in, argv[2], argv[3], err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    {
        char d[200];
        conv_describe(d, sizeof(d), &in);
        fprintf(stderr, "  %s\n", d);
    }
    puts(args);
    return 0;
}
