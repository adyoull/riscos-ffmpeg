/*
 * ff_riscos_relative_name (FFmpeg patch 0019, libavformat/riscos_filename.h):
 * which names typed on RISC OS get "@." (the current directory) in front, so
 * UnixLib reads holiday/mp4 as the RISC OS file holiday/mp4 and not as the
 * file mp4 in the directory holiday. Host gcc.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "riscos_filename.h"

static int known(const char *ext)
{
    static const char *const exts[] = { "mp4", "mkv", "avi", "mov", "m4a", "mp3", "webm", "ts", "mpg", "wav", NULL };
    for (int i = 0; exts[i]; i++)
        if (!strcasecmp(ext, exts[i]))
            return 1;
    return 0;
}

int main(void)
{
    static const char *const cases[][2] = {
        { "holiday/mp4",               "@.holiday/mp4" },       /* the RISC OS way */
        { "small/MKV",                 "@.small/MKV" },
        { "Films.holiday/mp4",         "@.Films.holiday/mp4" }, /* a RISC OS subdirectory */
        { "clip.mp4",                  "clip.mp4" },            /* Unix names: UnixLib's job */
        { "dir/clip.mp4",              "dir/clip.mp4" },
        { "videos/clip",               "videos/clip" },         /* not an extension */
        { "notes/txt",                 "notes/txt" },
        { "a/b/mp4",                   "a/b/mp4" },             /* more than one '/' */
        { "/abs/clip/mp4",             "/abs/clip/mp4" },
        { "@.holiday/mp4",             "@.holiday/mp4" },       /* RISC OS prefixes: already fine */
        { "$.Films.holiday/mp4",       "$.Films.holiday/mp4" },
        { "^.holiday/mp4",             "^.holiday/mp4" },
        { "<Obey$Dir>.clip/mp4",       "<Obey$Dir>.clip/mp4" },
        { "NVMe::Disc.$.clip/mp4",     "NVMe::Disc.$.clip/mp4" },
        { "RAM:clip/mp4",              "RAM:clip/mp4" },
        { "Films:clip/mp4",            "Films:clip/mp4" },
        { "pipe:1",                    "pipe:1" },
        { "-",                         "-" },
        { "clip",                      "clip" },
        { "/mp4",                      "/mp4" },
        { "clip/",                     "clip/" },
        { ".hidden/mp4",               ".hidden/mp4" },
        { "clip/mp4.bak",              "clip/mp4.bak" },
        { "clip/m-4",                  "clip/m-4" },
    };
    int fails = 0;
    char buf[64], small[8];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *r = ff_riscos_relative_name(cases[i][0], buf, sizeof(buf), known);
        if (strcmp(r, cases[i][1])) {
            printf("FAIL: %s -> %s (want %s)\n", cases[i][0], r, cases[i][1]);
            fails++;
        }
    }
    /* too long for the buffer: left as it was */
    if (strcmp(ff_riscos_relative_name("holiday/mp4", small, sizeof(small), known), "holiday/mp4"))
        printf("FAIL: a name too long for the buffer was changed\n"), fails++;
    printf(fails ? "%d FAILED\n" : "all passed (%d names)\n", fails ? fails : (int)(sizeof(cases) / sizeof(cases[0])));
    return !!fails;
}
