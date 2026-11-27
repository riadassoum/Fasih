"""md2html.py IN.md OUT.html LANG: the add-on's help page from a README, at build time.

Covers what the READMEs use: headings, paragraphs, bullet and numbered lists, tables, bold,
italics, inline code, code blocks and links. No dependencies, so a clean checkout builds.
"""
import html, re, sys


def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<![*\w])\*([^*\n]+)\*(?![*\w])", r"<em>\1</em>", t)
    t = re.sub(r"\[([^\]]+)\]\((https?://[^)\s]+)\)", r'<a href="\2">\1</a>', t)
    t = re.sub(r"(?<![\"'>=])(https?://[^\s<)]+)", r'<a href="\1">\1</a>', t)
    return t


def convert(md, lang):
    out, para, lst, table, code = [], [], None, [], None

    def flush():
        nonlocal para, lst, table
        if para:
            out.append("<p>%s</p>" % inline(" ".join(para))); para = []
        if lst:
            out.append("<%s>%s</%s>" % (lst[0], "".join("<li>%s</li>" % inline(i) for i in lst[1]), lst[0])); lst = None
        if table:
            rows = [r for r in table if not re.fullmatch(r"\|?[\s:|-]+\|?", r)]
            cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows]
            h = "<tr>%s</tr>" % "".join("<th>%s</th>" % inline(c) for c in cells[0])
            b = "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c) for c in r) for r in cells[1:])
            out.append("<table>%s%s</table>" % (h, b)); table = []

    for line in md.splitlines():
        if code is not None:
            if line.startswith("```"):
                out.append("<pre><code>%s</code></pre>" % html.escape("\n".join(code))); code = None
            else:
                code.append(line)
            continue
        if line.startswith("```"):
            flush(); code = []; continue
        s = line.strip()
        m = re.match(r"(#{1,6})\s+(.*)", s)
        if m:
            flush(); n = len(m.group(1)); out.append("<h%d>%s</h%d>" % (n, inline(m.group(2)), n)); continue
        if s.startswith("|"):
            if para or lst:
                flush()
            table.append(s); continue
        m = re.match(r"([-*]|\d+\.)\s+(.*)", s)
        if m and not line.startswith("    "):
            kind = "ol" if m.group(1)[0].isdigit() else "ul"
            if para or table or (lst and lst[0] != kind):
                flush()
            lst = lst or (kind, [])
            lst[1].append(m.group(2)); continue
        if not s:
            flush(); continue
        if lst and line.startswith(("  ", "\t")):
            lst[1][-1] += " " + s; continue
        if lst or table:
            flush()
        para.append(s)
    flush()
    d = ' dir="rtl"' if lang == "ar" else ""
    title = re.search(r"<h1>(.*?)</h1>", "".join(out))
    return ('<!DOCTYPE html>\n<html lang="%s"%s><head><meta charset="utf-8"><title>%s</title></head><body>\n%s\n</body></html>\n'
            % (lang, d, re.sub("<[^>]+>", "", title.group(1)) if title else "Fasih", "\n".join(out)))


if __name__ == "__main__":
    src, dst, lang = sys.argv[1], sys.argv[2], sys.argv[3]
    open(dst, "w", encoding="utf-8").write(convert(open(src, encoding="utf-8").read(), lang))
