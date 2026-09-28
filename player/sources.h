/*
 * sources.h - what Reel plays: files, and web addresses given in text.
 *
 * Reel is given a file (dropped, double-clicked, dragged from another
 * program, or a name on its command line). If it's a video, that's what
 * plays. If it's text, the addresses in it do:
 *   - a web address on its own (a URI file, &F91; an ANT URL file, &B28;
 *     a text file with one address);
 *   - several addresses, one a line: a playlist; M3U's #EXTINF titles are
 *     used (an M3U8 with #EXT-X- lines is an HLS playlist: played itself);
 *   - yt-dlp's output. "yt-dlp -g" prints the address to play, or two: the
 *     video's and the sound's (when the best video and best sound come
 *     apart, "bestvideo+bestaudio"; recognised by YouTube's mime=video /
 *     mime=audio). "yt-dlp -j" (or --dump-json) prints everything as JSON:
 *     the title, the address(es), and the HTTP headers the site wants; one
 *     line per video for a playlist (-J gives one object with "entries").
 * Part of riscos-ffmpeg (Reel). GPL v2 or later.
 */
#ifndef REEL_SOURCES_H
#define REEL_SOURCES_H
#include <stddef.h>

typedef struct source {
    char *url;          /* file name or address */
    char *audio_url;    /* the sound from another address, or NULL */
    char *headers;      /* HTTP headers, "Name: value\r\n"..., or NULL */
    char *user_agent;   /* or NULL */
    char *title;        /* Latin-1, or NULL */
    char *key;          /* what "carry on from where you stopped" remembers it by
                           (yt-dlp's webpage_url), or NULL: the url */
} source_t;

/* Reads text: the sources in it, up to max; 0 if there are no addresses in
   it. *hls: it's an HLS playlist (#EXT-X-): play the file itself. */
int sources_parse(const char *text, size_t len, source_t *out, int max, int *hls);

/* A file given to Reel: 0 play the file itself (a video, or an HLS
   playlist); n > 0 play these sources; -1 it can't be read; -2 it's text
   with no addresses in it. */
int sources_from_file(const char *path, source_t *out, int max);

/* A source for a file name or address on its own */
int source_simple(source_t *s, const char *url);
int source_copy(source_t *dst, const source_t *src);
void source_free(source_t *s);

/* What to show for it: the title, else the file's leaf name or the
   address's host and last part */
void source_name(const source_t *s, char *buf, size_t size);

/* The key for resume lists: at most size-1 characters (long addresses are
   hashed) */
void source_key(const source_t *s, char *buf, size_t size);

/* UTF-8 to Latin-1 (RISC OS's desktop font): common punctuation to its
   nearest, anything else outside Latin-1 to '?'. In place. */
void utf8_to_latin1(char *s);

#endif
