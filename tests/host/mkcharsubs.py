#!/usr/bin/env python3
"""SubRip files in the code pages people save them in (sub_test): Greek
(Windows-1253), Cyrillic (Windows-1251), Western (Windows-1252), Turkish
(Windows-1254), each two lines at 1-2.5 s and 3-4 s.

  mkcharsubs.py DIR"""
import sys, os
d = sys.argv[1]
SUBS = {
    "greek.srt": ("cp1253", "Καλημέρα, τι κάνεις;", "Πολύ καλά, ευχαριστώ!"),
    "cyr.srt": ("cp1251", "Привет, как дела?", "Хорошо, спасибо!"),
    "west.srt": ("cp1252", "Ça va? Très bien — café", "À bientôt!"),
    "turk.srt": ("cp1254", "Günaydın, nasılsın?", "Şimdi iyiyim, teşekkürler."),
}
for name, (cs, a, b) in SUBS.items():
    text = "1\r\n00:00:01,000 --> 00:00:02,500\r\n%s\r\n\r\n2\r\n00:00:03,000 --> 00:00:04,000\r\n%s\r\n\r\n" % (a, b)
    open(os.path.join(d, name), "wb").write(text.encode(cs))
