/*
 * Reel's sources (player/sources.c): the addresses in text Reel is given.
 * On the host (plain C). Each case is text as the programs make it: a URI
 * file, an ANT URL file, a list, an M3U with titles, an HLS playlist,
 * yt-dlp -g (one address, and bestvideo+bestaudio's two), yt-dlp -j (one
 * format; requested_formats; http_headers; a UTF-8 title), -j of a
 * playlist (one object a line), -J (entries), and things that aren't.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sources.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define S(x) ((x) ? (x) : "(null)")

static int parse(const char *text, source_t *out, int max, int *hls)
{
    return sources_parse(text, strlen(text), out, max, hls);
}

static void free_all(source_t *s, int n) { for (int i = 0; i < n; i++) source_free(&s[i]); }

int main(void)
{
    source_t s[8];
    int n, hls;
    char buf[128];

    /* a URI file (&F91) */
    n = parse("URI\t100\n\t# Made by a browser\n\thttps://example.com/films/clip.mp4\n", s, 8, &hls);
    CHECK(n == 1 && !strcmp(s[0].url, "https://example.com/films/clip.mp4") && !hls, "URI file: %d %s", n, n ? s[0].url : "");
    source_name(&s[0], buf, sizeof(buf));
    CHECK(!strcmp(buf, "example.com: clip.mp4"), "name: %s", buf);
    free_all(s, n);

    /* an ANT URL file (&B28) / one address, CR LF */
    n = parse("http://tv.example/live/stream.m3u8\r\n", s, 8, &hls);
    CHECK(n == 1 && !strcmp(s[0].url, "http://tv.example/live/stream.m3u8"), "URL file: %d", n);
    free_all(s, n);

    /* a list: a playlist; junk lines ignored */
    n = parse("Some films:\nhttps://a.example/1.mp4\n\nnot an address\nhttps://b.example/2.webm\nftp:/x\n", s, 8, &hls);
    CHECK(n == 2 && !strcmp(s[1].url, "https://b.example/2.webm") && !s[0].audio_url, "list: %d", n);
    free_all(s, n);

    /* M3U with titles (UTF-8) */
    n = parse("#EXTM3U\n#EXTINF:123,Caf\xC3\xA9 \xE2\x80\x9CNight\xE2\x80\x9D\nhttp://r.example/a.mp3\n"
              "#EXTINF:-1,Second\nhttp://r.example/b.mp4\n", s, 8, &hls);
    CHECK(n == 2 && s[0].title && !strcmp(s[0].title, "Caf\xE9 \"Night\"") && !strcmp(S(s[1].title), "Second"),
          "M3U: %d '%s' '%s'", n, n ? S(s[0].title) : "", n > 1 ? S(s[1].title) : "");
    free_all(s, n);

    /* HLS playlist: played itself */
    n = parse("#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:10,\nhttps://cdn.example/seg1.ts\n", s, 8, &hls);
    CHECK(n == 0 && hls, "HLS: %d hls %d", n, hls);

    /* yt-dlp -g, one format */
    n = parse("https://rr1.googlevideo.com/videoplayback?expire=1&itag=22&mime=video%2Fmp4&x=1\n", s, 8, &hls);
    CHECK(n == 1 && !s[0].audio_url, "-g one: %d", n);
    free_all(s, n);

    /* yt-dlp -g, bestvideo+bestaudio: one source, video + sound */
    n = parse("https://rr1.googlevideo.com/videoplayback?itag=136&mime=video%2Fmp4&x=1\n"
              "https://rr1.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4&x=2\n", s, 8, &hls);
    CHECK(n == 1 && s[0].audio_url && strstr(s[0].url, "itag=136") && strstr(s[0].audio_url, "itag=140"),
          "-g pair: %d", n);
    free_all(s, n);

    /* yt-dlp -j: requested_formats (video, then sound), http_headers, title, webpage_url */
    n = parse("{\"id\": \"abc\", \"title\": \"A \\u201cfilm\\u201d \\u00e9t\\u00e9 \\ud83d\\ude00\", "
              "\"webpage_url\": \"https://www.youtube.com/watch?v=abc\", \"duration\": 12.5, \"is_live\": false, "
              "\"formats\": [{\"url\": \"https://x/unused\"}], "
              "\"requested_formats\": ["
              "{\"format_id\": \"136\", \"url\": \"https://v.example/video?a=1\\u0026b=2\", \"vcodec\": \"avc1.4d401f\", \"acodec\": \"none\", "
              "\"http_headers\": {\"User-Agent\": \"Mozilla/5.0 (X11)\", \"Accept\": \"*/*\", \"Sec-Fetch-Mode\": \"navigate\"}},"
              "{\"format_id\": \"140\", \"url\": \"https://v.example/audio\", \"vcodec\": \"none\", \"acodec\": \"mp4a.40.2\"}],"
              "\"http_headers\": {\"User-Agent\": \"top\"}, \"thumbnails\": [], \"n\": null}", s, 8, &hls);
    CHECK(n == 1, "-j: %d", n);
    if (n == 1) {
        CHECK(!strcmp(s[0].url, "https://v.example/video?a=1&b=2") && !strcmp(S(s[0].audio_url), "https://v.example/audio"),
              "-j urls: %s %s", s[0].url, S(s[0].audio_url));
        CHECK(!strcmp(S(s[0].user_agent), "Mozilla/5.0 (X11)") &&
              !strcmp(S(s[0].headers), "Accept: */*\r\nSec-Fetch-Mode: navigate\r\n"), "-j headers: '%s' '%s'",
              S(s[0].user_agent), S(s[0].headers));
        CHECK(!strcmp(S(s[0].title), "A \"film\" \xE9t\xE9 ?"), "-j title: '%s'", S(s[0].title));
        CHECK(!strcmp(S(s[0].key), "https://www.youtube.com/watch?v=abc"), "-j key: %s", S(s[0].key));
    }
    free_all(s, n);

    /* yt-dlp -j, a single format with its own url */
    n = parse("{\"title\": \"One\", \"url\": \"https://cdn.example/one.m3u8\", \"http_headers\": {\"Referer\": \"https://site/\"}}",
              s, 8, &hls);
    CHECK(n == 1 && !strcmp(s[0].url, "https://cdn.example/one.m3u8") && !s[0].audio_url &&
          !strcmp(S(s[0].headers), "Referer: https://site/\r\n") && !s[0].user_agent, "-j single: %d", n);
    free_all(s, n);

    /* yt-dlp -j of a playlist: one object a line */
    n = parse("{\"title\": \"A\", \"url\": \"https://a/1\"}\n{\"title\": \"B\", \"url\": \"https://a/2\"}\n", s, 8, &hls);
    CHECK(n == 2 && !strcmp(S(s[1].title), "B"), "-j lines: %d", n);
    free_all(s, n);

    /* -J: entries; a flat entry (a web page) is skipped */
    n = parse("{\"_type\": \"playlist\", \"entries\": [{\"title\": \"A\", \"url\": \"https://a/1\"},"
              "{\"_type\": \"url\", \"url\": \"https://www.youtube.com/watch?v=x\"},"
              "{\"title\": \"C\", \"requested_formats\": [{\"url\": \"https://a/3v\", \"vcodec\": \"vp9\"}]}]}", s, 8, &hls);
    CHECK(n == 2 && !strcmp(S(s[1].title), "C") && !strcmp(s[1].url, "https://a/3v"), "-J: %d", n);
    free_all(s, n);

    /* not addresses; broken JSON; too many */
    n = parse("Just some notes\nwith no links\n", s, 8, &hls);
    CHECK(n == 0 && !hls, "notes: %d", n);
    n = parse("{\"title\": \"broken\", \"url\": ", s, 8, &hls);
    CHECK(n == 0, "broken JSON: %d", n);
    n = parse("http://1/\nhttp://2/\nhttp://3/\n", s, 2, &hls);
    CHECK(n == 2, "max 2: %d", n);
    free_all(s, n);

    /* names and keys */
    {
        source_t t;
        source_simple(&t, "SDFS::Pi.$.Films.holiday/mp4");
        source_name(&t, buf, sizeof(buf));
        CHECK(!strcmp(buf, "holiday/mp4"), "file name: %s", buf);
        source_free(&t);
        source_simple(&t, "https://host.example/");
        source_name(&t, buf, sizeof(buf));
        CHECK(!strcmp(buf, "host.example"), "host only: %s", buf);
        source_free(&t);
        source_simple(&t, "https://rr1.googlevideo.com/videoplayback?itag=22&a=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        source_key(&t, buf, 64);
        CHECK(strlen(buf) == 63 && strchr(buf, '#'), "long key: %s", buf);
        source_free(&t);
    }

    /* files: a video isn't text; text without addresses */
    {
        FILE *f = fopen("/tmp/reel_src_bin", "wb");
        fwrite("\0\0\0\x20" "ftypisom", 1, 12, f);
        fclose(f);
        CHECK(sources_from_file("/tmp/reel_src_bin", s, 8) == 0, "binary file isn't a video to play");
        f = fopen("/tmp/reel_src_txt", "wb");
        fputs("no links here\n", f);
        fclose(f);
        CHECK(sources_from_file("/tmp/reel_src_txt", s, 8) == -2, "text with no addresses");
        f = fopen("/tmp/reel_src_url", "wb");
        fputs("https://a.example/x.mp4\n", f);
        fclose(f);
        n = sources_from_file("/tmp/reel_src_url", s, 8);
        CHECK(n == 1, "URL file: %d", n);
        free_all(s, n > 0 ? n : 0);
        CHECK(sources_from_file("/tmp/no/such/file", s, 8) == -1, "missing file");
        remove("/tmp/reel_src_bin"); remove("/tmp/reel_src_txt"); remove("/tmp/reel_src_url");
    }

    /* HLS playlists saved from the web: relative names, and rebasing them */
    {
        const char *master =
            "#EXTM3U\n#EXT-X-VERSION:4\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",URI=\"audio/en.m3u8\"\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,AUDIO=\"a\"\r\n"
            "chunklist_360.m3u8?token=1\r\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=3000000\n"
            "/live/abs_1080.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=4000000\n"
            "//cdn.example/other.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=5000000\n"
            "https://cdn.example/full.m3u8\n";
        const char *whole = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nhttps://a.example/v.m3u8\n"
                            "#EXT-X-MEDIA:TYPE=AUDIO,URI=\"https://a.example/a.m3u8\"\n";
        char *r;
        CHECK(sources_hls_relative(master), "relative names not seen");
        CHECK(!sources_hls_relative(whole), "whole addresses taken as relative");
        CHECK(!sources_hls_relative("chunk.m3u8\nother.mp4\n"), "not HLS, but called relative");
        CHECK(sources_hls_relative("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\nhttps://a/s1.m4s\n"), "a relative MAP URI not seen");
        r = sources_hls_rebase(master, "https://tv.example/live/stream/master.m3u8?session=9");
        CHECK(r && strstr(r, "URI=\"https://tv.example/live/stream/audio/en.m3u8\"\n"), "MEDIA URI: %s", r ? r : "");
        CHECK(r && strstr(r, "\nhttps://tv.example/live/stream/chunklist_360.m3u8?token=1\n"), "relative line: %s", r ? r : "");
        CHECK(r && strstr(r, "\nhttps://tv.example/live/abs_1080.m3u8\n"), "host-relative line: %s", r ? r : "");
        CHECK(r && strstr(r, "\nhttps://cdn.example/other.m3u8\n"), "scheme-relative line: %s", r ? r : "");
        CHECK(r && strstr(r, "\nhttps://cdn.example/full.m3u8\n"), "whole address changed: %s", r ? r : "");
        CHECK(r && strstr(r, "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,AUDIO=\"a\"\n"), "tag line changed: %s", r ? r : "");
        CHECK(r && !sources_hls_relative(r), "still relative after rebasing:\n%s", r ? r : "");
        free(r);
        r = sources_hls_rebase(master, "http://tv.example");
        CHECK(r && strstr(r, "\nhttp://tv.example/chunklist_360.m3u8?token=1\n"), "no path: %s", r ? r : "");
        free(r);
        r = sources_hls_rebase(master, "http://tv.example/dir/");
        CHECK(r && strstr(r, "\nhttp://tv.example/dir/chunklist_360.m3u8?token=1\n"), "a directory: %s", r ? r : "");
        free(r);
        CHECK(!sources_hls_rebase(master, "not an address"), "rebased against text");
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
