#!/usr/bin/env python3
"""Build the Kinoklip website (static, for GitHub Pages under /kinoklip/).

Uses only the Python standard library and the `markdown` module.

    python3 scripts/build_website.py              # writes _site/
    python3 scripts/build_website.py --out dir    # another output directory
    python3 scripts/build_website.py --check      # build, then check every link, image and tag
    python3 -m http.server -d _site               # look at it

Every link is relative, so the site works under any base path. Only 404.html needs an
absolute base (--base, default /kinoklip/).
"""
import argparse
import html
import json
import os
import re
import shutil
import sys
from html.parser import HTMLParser

try:
    import markdown
except ImportError:
    sys.exit("The 'markdown' module is missing: pip install markdown")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "website")
GUIDE = os.path.join(REPO, "docs", "guide")
RELEASES = os.path.join(REPO, "docs", "releases")
SHOTS = os.path.join(REPO, "docs", "images", "readme")  # the readme's screenshots, also used on the site
GITHUB = "https://github.com/SeniorMatt/kinoklip"

# The guide: a short path for beginners first, then reference groups. Anything not listed ends up under "More".
START = ["install-kinoklip", "getting-started", "import-your-clips", "cut-and-arrange-clips", "colour-correct-a-clip",
         "export-for-youtube"]
# Shown on the guide's front page as the tasks people ask for most (also listed in their group).
COMMON = ["add-a-title", "transitions", "fade-audio-and-add-music", "change-clip-speed", "remove-a-green-screen",
          "add-subtitles", "vertical-video", "smooth-playback"]
GROUPS = [
    ("Start here", START),
    ("Editing", ["add-a-title", "transitions", "fade-to-black", "change-clip-speed", "freeze-a-frame",
                 "zoom-crop-and-picture-in-picture", "keyframe-animation", "stabilize-a-shaky-clip",
                 "track-and-warp-a-picture", "hide-part-of-the-picture", "vertical-video",
                 "change-the-frame-rate-of-a-timeline"]),
    ("Titles and subtitles", ["add-layers-to-a-title", "emoji-in-titles-and-captions", "type-text-on-with-write-on",
                              "add-subtitles"]),
    ("Effects and looks", ["test-patterns-and-looks", "camera-and-screen-looks", "sharpen-repair-and-damage-effects",
                           "effects-that-use-other-frames"]),
    ("Color and compositing", ["change-the-colour-of-one-object", "use-a-lut", "use-agx", "compositing-page",
                               "remove-a-green-screen", "remove-a-background"]),
    ("Audio", ["record-a-voice-over", "fade-audio-and-add-music", "even-out-clip-volumes",
               "change-pitch-and-equalize-a-clip", "separate-audio-from-video"]),
    ("Export and exchange", ["move-a-timeline-to-another-editor"]),
    ("Keyboard", ["use-the-shortcuts-of-your-old-editor", "vim-quick-start", "vim-cheat-sheet", "edit-with-vim-keys"]),
    ("Automation", ["control-kinoklip-from-the-command-line", "control-kinoklip-from-an-ai-assistant"]),
    ("Settings and troubleshooting", ["smooth-playback", "send-a-log-file-with-a-bug-report",
                                      "move-your-settings-to-another-computer"]),
]
# Old article ids that were merged into another one keep a page that sends the reader on: {old id: new id}.
# "readme" is the guide's start page in the app's index.
REDIRECTS = {"readme": ""}
# The screenshots of the app that the site's own pages use (from docs/images/readme).
SITE_SHOTS = ["edit.png", "color.png", "compositing.png"]

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr", "path", "circle", "rect", "line", "stop"}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


# ---------------------------------------------------------------- templates

def version_info():
    m = re.search(r"project\([^)]*VERSION\s+(\d+)\.(\d+)\.(\d+)", read(os.path.join(REPO, "CMakeLists.txt")))
    if not m:
        sys.exit("No VERSION in CMakeLists.txt")
    a, b, c = m.groups()
    full = "%s.%s.%s" % (a, b, c)
    short = "%s.%s" % (a, b) if c == "0" else full
    return full, short


class Site:
    def __init__(self, out, base):
        self.out = out
        self.base = base
        self.version, self.version_short = version_info()
        self.partials = {}
        pdir = os.path.join(SRC, "partials")
        for name in os.listdir(pdir):
            self.partials[os.path.splitext(name)[0]] = read(os.path.join(pdir, name)).rstrip("\n")
        self.layout = read(os.path.join(SRC, "layout.html"))

    def expand(self, text, depth=0):
        def sub(m):
            name = m.group(1)
            if name not in self.partials:
                sys.exit("Unknown partial: " + name)
            return self.expand(self.partials[name], depth + 1) if depth < 5 else ""
        return re.sub(r"\{\{>\s*([\w-]+)\s*\}\}", sub, text)

    def fill(self, text, ctx):
        text = self.expand(text)

        def sub(m):
            if m.group(1) not in ctx:
                sys.exit("Unknown placeholder: " + m.group(0))
            return ctx[m.group(1)]
        return re.sub(r"\{\{(\w+)\}\}", sub, text)

    def page(self, rel, title, description, body, nav=None, root=None, body_is_template=False, extra=None):
        """Write one page. rel is the output path like 'features/index.html'."""
        depth = rel.count("/")
        root = root if root is not None else "../" * depth
        ctx = {"root": root, "version": self.version, "version_short": self.version_short}
        for k in ("features", "guide", "download", "report", "releases"):
            ctx["cur_" + k] = ' aria-current="page"' if nav == k else ""
        ctx.update(extra or {})
        if body_is_template:
            body = self.fill(body, ctx)
        header = self.fill(self.partials["header"], ctx)
        footer = self.fill(self.partials["footer"], ctx)
        page = self.layout
        for k, v in (("title", html.escape(title)), ("description", html.escape(description, quote=True)), ("root", root),
                     ("header", header), ("footer", footer)):
            page = page.replace("{{%s}}" % k, v)
        page = page.replace("{{body}}", body)
        write(os.path.join(self.out, rel), page)


# ---------------------------------------------------------------- markdown

def gh_slug(value, sep="-"):
    value = re.sub(r"<[^>]+>", "", value).lower().strip()
    value = re.sub(r"[^\w\- ]", "", value, flags=re.UNICODE)
    return value.replace(" ", sep)


LIST_RE = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+")


def normalize_md(text):
    """GitHub accepts 2 and 3 space list indents and lists right after a paragraph; Python-Markdown wants 4 and a blank line."""
    lines = text.split("\n")
    out = []
    stack = [0]
    fence = None  # (old_indent, new_indent)
    prev = ""
    for line in lines:
        stripped = line.strip()
        indent = len(line) - len(line.lstrip(" "))
        if fence is not None:
            if stripped.startswith("```"):
                out.append(" " * fence[1] + stripped)
                fence = None
            else:
                out.append(" " * fence[1] + line[min(indent, fence[0]):] if stripped else "")
            prev = line
            continue
        if not stripped:
            out.append("")
            prev = ""
            continue
        while stack[-1] > indent:
            stack.pop()
        if indent > stack[-1]:
            stack.append(indent)
        new = 4 * (len(stack) - 1) + (indent - stack[-1])
        if indent == 0:
            stack = [0]
        is_list = bool(LIST_RE.match(line))
        is_table = stripped.startswith("|")
        if prev.strip() and (is_list or is_table) and not LIST_RE.match(prev) and not prev.lstrip().startswith("|") \
                and (len(prev) - len(prev.lstrip(" "))) == 0 and indent == 0 and out and out[-1] != "":
            out.append("")
        if is_list and prev.strip() and (len(prev) - len(prev.lstrip(" "))) > indent and out and out[-1] != "":
            out.append("")
        if stripped.startswith("```"):
            fence = (indent, new)
            out.append(" " * new + stripped)
        else:
            out.append(" " * new + line.lstrip(" "))
        prev = line
    return "\n".join(out)


def render_md(text):
    md = markdown.Markdown(extensions=["tables", "fenced_code", "toc", "sane_lists", "attr_list"],
                           extension_configs={"toc": {"slugify": gh_slug, "permalink": False}})
    out = md.convert(normalize_md(text))
    toc = []
    for t in md.toc_tokens:
        if t["level"] == 1:
            for c in t["children"]:
                toc.append((c["id"], c["name"], c["level"]))
            continue
        toc.append((t["id"], t["name"], t["level"]))
        for c in t.get("children", []):
            if c["level"] == 3:
                toc.append((c["id"], c["name"], c["level"]))
    md.reset()
    return out, toc


class LinkFixer:
    """Rewrites links in rendered markdown: guide articles to site pages, images to copied files, other repo files to GitHub."""

    def __init__(self, src_dir, root, article_files, warnings):
        self.src_dir = src_dir
        self.root = root
        self.article_files = article_files  # abs path -> id
        self.warnings = warnings

    def fix(self, target, is_img):
        if re.match(r"^([a-z][a-z0-9+.-]*:|#|//)", target, re.I):
            return target
        path, _, frag = target.partition("#")
        frag = "#" + frag if frag else ""
        if not path:
            return target
        full = os.path.normpath(os.path.join(self.src_dir, html.unescape(path)))
        if full in self.article_files:
            return "%sguide/%s/%s" % (self.root, self.article_files[full], frag)
        if full == os.path.join(GUIDE, "readme.md"):
            return "%sguide/%s" % (self.root, frag)
        gi = os.path.join(GUIDE, "images") + os.sep
        if is_img and full.startswith(gi):
            return "%sguide/images/%s" % (self.root, os.path.relpath(full, gi))
        rel = os.path.relpath(full, REPO)
        if rel.startswith(".."):
            self.warnings.append("link leaves the repo: " + target)
            return target
        if not os.path.exists(full):
            self.warnings.append("missing repo file: %s (from %s)" % (rel, self.src_dir))
        kind = "tree" if os.path.isdir(full) else "blob"
        return "%s/%s/main/%s%s" % (GITHUB, kind, rel.replace(os.sep, "/"), frag)

    def apply(self, htmltext):
        def sub(m):
            attr, q, val = m.group(1), m.group(2), m.group(3)
            return '%s=%s%s%s' % (attr, q, self.fix(val, attr == "src"), q)
        return re.sub(r'\b(href|src)=(["\'])(.*?)\2', sub, htmltext)


# ---------------------------------------------------------------- guide

def plain_text(htmltext):
    t = re.sub(r"<(script|style)[^>]*>.*?</\1>", " ", htmltext, flags=re.S)
    t = re.sub(r"<[^>]+>", " ", t)
    return re.sub(r"\s+", " ", html.unescape(t)).strip()


def build_guide(site, warnings):
    index = json.loads(read(os.path.join(GUIDE, "index.json")))
    articles = index["articles"]
    # An article that is not in index.json yet still gets a page (title and summary taken from the file).
    known = {a["file"] for a in articles}
    for f in sorted(os.listdir(GUIDE)):
        if f.endswith(".md") and f not in known and f not in ("readme.md", "topics.md"):
            text = read(os.path.join(GUIDE, f))
            title = re.match(r"\s*# (.+)", text)
            para = re.search(r"^(?!#)(\S.*(?:\n\S.*)*)", text.split("\n", 1)[1].lstrip(), re.M)
            summary = re.sub(r"\s+", " ", para.group(1)).split(". ")[0].rstrip(".") + "." if para else ""
            summary = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", summary).replace("**", "")
            articles.append({"id": f[:-3], "title": title.group(1) if title else f[:-3], "file": f, "summary": summary, "keywords": []})
            warnings.append("%s is not in index.json; using its first paragraph as summary" % f)
    by_id = {a["id"]: a for a in articles}
    article_files = {os.path.join(GUIDE, a["file"]): a["id"] for a in articles}
    groups = []
    seen = set()
    for name, ids in GROUPS:
        for i in ids:
            if i not in by_id:
                warnings.append("guide group names unknown article: " + i)
        ids = [i for i in ids if i in by_id]
        groups.append((name, ids))
        seen.update(ids)
    rest = [a["id"] for a in articles if a["id"] not in seen]
    if rest:
        groups.append(("More", rest))
    start = [i for i in START if i in by_id]

    shutil.copytree(os.path.join(GUIDE, "images"), os.path.join(site.out, "guide", "images"), dirs_exist_ok=True)

    # the page of every article, rendered first so the search index can use the text
    pages = {}
    for aid, a in by_id.items():
        src = read(os.path.join(GUIDE, a["file"]))
        src = re.sub(r"\A\s*# .*\n", "", src, count=1)
        fixer = LinkFixer(GUIDE, "../../", article_files, warnings)
        content, toc = render_md(src)
        pages[aid] = (fixer.apply(content), toc)

    # search index
    data = []
    for aid, a in by_id.items():
        content, toc = pages[aid]
        data.append({"id": aid, "title": a["title"], "summary": a["summary"], "keywords": a.get("keywords", []),
                     "headings": [{"t": re.sub(r"<[^>]+>", "", n), "a": i} for i, n, lv in toc],
                     "text": plain_text(content)})
    write(os.path.join(site.out, "guide", "search.json"), json.dumps(data, ensure_ascii=False, separators=(",", ":")))

    # front page of the guide
    def link(i):
        return '<a href="%s/">%s</a>' % (i, html.escape(by_id[i]["title"]))
    path = "\n".join('<li><div><a href="%s/">%s</a><span>%s</span></div></li>'
                     % (i, html.escape(by_id[i]["title"]), html.escape(by_id[i]["summary"])) for i in start)
    common = "\n".join("<li>%s</li>" % link(i) for i in COMMON if i in by_id)
    ref = []
    for name, ids in groups:
        if name == "Start here":
            continue
        ref.append('<section><h3>%s</h3><ul>%s</ul></section>' % (html.escape(name), "".join("<li>%s</li>" % link(i) for i in ids)))
    body = """<section class="guide-head"><div class="wrap">
<h1>Guide</h1>
<p class="lead">How to use Kinoklip, one task at a time. Kinoklip is laid out like DaVinci Resolve and has the same shortcuts, so most Resolve tutorials for the Edit page work here too.</p>
<div class="search" role="search"><label for="guide-q" class="sr" style="position:absolute;left:-9999px">Search the guide</label><input id="guide-q" type="search" data-search data-autofill placeholder="Search the guide: green screen, export, LUT, voice-over" autocomplete="off"><ul class="results" hidden></ul></div>
<p class="muted small" style="margin-top:8px">Press <kbd>/</kbd> on any guide page to search. In Kinoklip, <kbd>F1</kbd> opens the article about what you are working on.</p>
</div></section>
<section class="section"><div class="wrap">
<h2>Start here</h2>
<p class="muted narrow">New to Kinoklip? Read these six in order. They take you from the download to a finished video.</p>
<ol class="path">
%s
</ol>
</div></section>
<section class="section"><div class="wrap">
<h2>Common tasks</h2>
<ul class="plain cols-list">
%s
</ul>
</div></section>
<section class="section"><div class="wrap">
<h2>Reference</h2>
<p class="muted narrow">Every article, by subject. Every shortcut is in the <a href="%s/blob/main/docs/keymap-resolve-windows.md">Resolve keymap</a>; the other editors' presets are in <a href="%s/blob/main/docs/keymaps/readme.md">keymaps</a>, and every menu entry is in the <a href="%s/blob/main/docs/menu-reference.md">menu reference</a>.</p>
<div class="refgroups">
%s
</div>
</div></section>""" % (path, common, GITHUB, GITHUB, GITHUB, "\n".join(ref))
    site.page("guide/index.html", "Guide - Kinoklip", "How to use Kinoklip: a short path from install to export, and articles by subject.", body, nav="guide")

    def group_of(aid):
        for name, ids in groups:
            if aid in ids:
                return name
        return ""

    def sidebar(current):
        cur_group = group_of(current)
        out = ['<aside class="side"><div class="search" role="search"><label for="side-q" style="position:absolute;left:-9999px">Search the guide</label>'
               '<input id="side-q" type="search" data-search placeholder="Search (press /)" autocomplete="off"><ul class="results" hidden></ul></div>'
               '<details class="navbox" open><summary>Guide contents</summary><nav aria-label="Guide articles"><a href="../">Guide home</a>']
        for name, ids in groups:
            opened = " open" if name == cur_group or name == "Start here" else ""
            out.append("<details%s><summary>%s</summary>" % (opened, html.escape(name)))
            for i in ids:
                cur = ' aria-current="page"' if i == current and name == cur_group else ""
                out.append('<a href="../%s/"%s>%s</a>' % (i, cur, html.escape(by_id[i]["title"])))
            out.append("</details>")
        out.append("</nav></details></aside>")
        return "\n".join(out)

    for aid, a in by_id.items():
        content, toc = pages[aid]
        in_start = aid in start
        if in_start:
            n = start.index(aid)
            crumb = '<a href="../">Guide</a> / Start here, step %d of %d' % (n + 1, len(start))
            prev = by_id[start[n - 1]] if n > 0 else None
            nxt = by_id[start[n + 1]] if n + 1 < len(start) else None
            pager = '<nav class="pager" aria-label="Previous and next step">%s%s</nav>' % (
                '<a href="../%s/"><small>Previous</small>%s</a>' % (prev["id"], html.escape(prev["title"])) if prev else "<span></span>",
                '<a class="next" href="../%s/"><small>Next</small>%s</a>' % (nxt["id"], html.escape(nxt["title"])) if nxt else "")
        else:
            crumb = '<a href="../">Guide</a> / %s' % html.escape(group_of(aid) or "More")
            pager = ""
        heads = [(i, nme) for i, nme, lv in toc if lv == 2]
        toc_html = ""
        inline = ""
        if len(heads) >= 3:
            links = "".join('<a href="#%s">%s</a>' % (i, re.sub(r"<[^>]+>", "", nme)) for i, nme in heads)
            toc_html = '<aside class="toc" aria-label="On this page"><h2>On this page</h2>%s</aside>' % links
            inline = '<details class="toc-inline"><summary>On this page</summary>%s</details>' % links
        body = """<div class="wrap"><div class="guide-layout">
%s
<article class="prose">
<p class="crumb">%s</p>
<h1>%s</h1>
%s
%s
%s
<p class="edit-link">Found a mistake? <a href="%s/blob/main/docs/guide/%s">Edit this article on GitHub</a>.</p>
</article>
%s
</div></div>""" % (sidebar(aid), crumb, html.escape(a["title"]), inline, content, pager, GITHUB, a["file"], toc_html)
        site.page("guide/%s/index.html" % aid, "%s - Kinoklip guide" % a["title"], a["summary"], body, nav="guide")

    # redirects for ids that moved
    for old, new in REDIRECTS.items():
        if old in by_id:
            warnings.append("redirect id %s is also an article" % old)
            continue
        target = "../%s/" % new if new else "../"
        label = by_id[new]["title"] if new else "the guide"
        body = """<section class="page-head"><div class="wrap"><h1>This page has moved</h1>
<p class="lead">It is now <a href="%s">%s</a>.</p></div></section>""" % (target, html.escape(label))
        site.page("guide/%s/index.html" % old, "Moved - Kinoklip guide", "This guide page has moved.", body, nav="guide",
                  extra={})
        redirect_head(site, "guide/%s/index.html" % old, target)
    return by_id


def redirect_head(site, rel, target):
    path = os.path.join(site.out, rel)
    text = read(path)
    text = text.replace("<title>", '<meta http-equiv="refresh" content="0; url=%s">\n<link rel="canonical" href="%s">\n<title>' % (target, target), 1)
    write(path, text)


# ---------------------------------------------------------------- releases

def semver_key(name):
    m = re.match(r"v(\d+)\.(\d+)\.(\d+)(?:-([a-z]+)\.(\d+))?$", name)
    if not m:
        return None
    a, b, c, tag, num = m.groups()
    pre = 0 if tag else 1  # a pre-release sorts below its final release
    tagrank = {"alpha": 0, "beta": 1}.get(tag or "", 2)
    return (int(a), int(b), int(c), pre, tagrank, int(num or 0))


def release_list():
    items = []
    for f in os.listdir(RELEASES):
        base = os.path.splitext(f)[0]
        key = semver_key(base)
        if key:
            items.append((key, base, os.path.join(RELEASES, f)))
    items.sort(reverse=True)
    return items


def short_text(t):
    t = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", t)
    t = t.replace("**", "").replace("`", "").strip()
    if len(t) > 110:
        if ":" in t:
            t = t.split(":")[0]
        if len(t) > 110:
            t = t[:105].rsplit(" ", 1)[0] + "..."
    return t


def highlights(path):
    """Two short lists from the newest release notes: sections that start with 'New'."""
    secs = []
    cur = None
    for line in read(path).split("\n"):
        if line.startswith("### "):
            title = line[4:].strip()
            cur = {"title": title, "items": []} if title.startswith("New") else None
            if cur:
                secs.append(cur)
        elif line.startswith("## "):
            cur = None
        elif cur is not None and re.match(r"^[-*] ", line):
            cur["items"].append(short_text(line[2:]))
    out = []
    for s in secs[:2]:
        lis = "".join("<li>%s</li>" % html.escape(i) for i in s["items"][:5])
        out.append('<div><h3>%s</h3><ul>%s</ul></div>' % (html.escape(s["title"]), lis))
    return "\n".join(out)


def build_releases(site, article_files, warnings):
    items = release_list()
    parts = []
    for n, (key, base, path) in enumerate(items):
        text = read(path)
        fixer = LinkFixer(RELEASES, "../", article_files, warnings)
        content = fixer.apply(render_md(text)[0])
        ver = base[1:]
        first = re.search(r"^### (.+)$", text, re.M)
        sub = html.escape(re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", first.group(1))) if first else ""
        pill = ' <span class="pill">Latest</span>' if n == 0 else ""
        parts.append('<details class="rel" id="%s"%s><summary><b>%s</b>%s<span>%s</span></summary><div class="prose">%s</div></details>'
                     % (base, " open" if n == 0 else "", html.escape(ver), pill, sub, content))
    body = """<section class="page-head"><div class="wrap">
<h1>Release notes</h1>
<p class="lead">What changed in each version, newest first. To get a version, see the <a href="../download/">download page</a> or the <a href="%s/releases">releases on GitHub</a>.</p>
</div></section>
<section class="section" style="border-top:0"><div class="wrap">%s</div></section>""" % (GITHUB, "\n".join(parts))
    site.page("releases/index.html", "Release notes - Kinoklip", "What changed in each Kinoklip release.", body, nav="releases")
    return items


# ---------------------------------------------------------------- build

def build(out, base):
    warnings = []
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    site = Site(out, base)
    for d in ("css", "js", "static"):
        shutil.copytree(os.path.join(SRC, d), os.path.join(out, d))
    shots = os.path.join(out, "static", "shots")
    os.makedirs(shots)
    for f in SITE_SHOTS:
        shutil.copy(os.path.join(SHOTS, f), os.path.join(shots, f))
    write(os.path.join(out, ".nojekyll"), "")

    by_id = build_guide(site, warnings)
    article_files = {os.path.join(GUIDE, a["file"]): a["id"] for a in by_id.values()}
    items = build_releases(site, article_files, warnings)
    latest = highlights(items[0][2]) if items else ""

    def tpl(name):
        return read(os.path.join(SRC, "pages", name + ".html"))
    site.page("index.html", "Kinoklip: a free video editor for Linux, laid out like DaVinci Resolve",
              "Kinoklip is a free, open source Qt and FFmpeg video editor for Linux with DaVinci Resolve's layout and shortcuts.",
              tpl("home"), body_is_template=True, extra={"highlights": latest})
    site.page("features/index.html", "Features - Kinoklip", "Editing, compositing, color, audio, export, performance, Vim mode, command line and keymaps in Kinoklip.",
              tpl("features"), nav="features", body_is_template=True)
    site.page("download/index.html", "Download Kinoklip for Linux", "Download the Kinoklip AppImage, how to run it, and what your computer needs.",
              tpl("download"), nav="download", body_is_template=True)
    site.page("about/index.html", "About this project - Kinoklip", "What Kinoklip is, how it is made, and how to reach the people behind it.",
              tpl("about"), body_is_template=True)
    site.page("report-a-bug/index.html", "Report a bug - Kinoklip", "How to report a Kinoklip bug: save a log file, then open a prefilled GitHub issue.",
              tpl("report"), nav="report", body_is_template=True)
    notfound = """<section class="page-head"><div class="wrap"><h1>This page is not here</h1>
<p class="lead">The page may have moved. Try the <a href="{{root}}">home page</a> or the <a href="{{root}}guide/">guide</a>.</p></div></section>"""
    site.page("404.html", "Page not found - Kinoklip", "Page not found.", notfound, root=base, body_is_template=True)

    for w in sorted(set(warnings)):
        print("warning:", w)
    print("Built %s (Kinoklip %s): %d guide articles, %d releases" % (out, site.version, len(by_id), len(items)))
    return warnings


# ---------------------------------------------------------------- check

class Scan(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links = []
        self.ids = set()
        self.stack = []
        self.errors = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        for k in ("href", "src"):
            if k in a and a[k] is not None:
                self.links.append((k, a[k], self.getpos()[0]))
        if tag not in VOID:
            self.stack.append((tag, self.getpos()[0]))

    def handle_startendtag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        for k in ("href", "src"):
            if k in a and a[k] is not None:
                self.links.append((k, a[k], self.getpos()[0]))

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1][0] != tag:
            self.errors.append("line %d: unexpected </%s> (open: %s)" % (self.getpos()[0], tag, self.stack[-1] if self.stack else None))
            for i in range(len(self.stack) - 1, -1, -1):
                if self.stack[i][0] == tag:
                    del self.stack[i:]
                    break
        else:
            self.stack.pop()


def check(out, base):
    pages = {}
    for dp, _, files in os.walk(out):
        for f in files:
            if f.endswith(".html"):
                p = os.path.join(dp, f)
                s = Scan()
                s.feed(read(p))
                s.close()
                for tag, line in s.stack:
                    s.errors.append("line %d: <%s> never closed" % (line, tag))
                pages[p] = s
    bad = 0
    for p, s in sorted(pages.items()):
        rel = os.path.relpath(p, out)
        for e in s.errors:
            print("%s: %s" % (rel, e))
            bad += 1
        for kind, link, line in s.links:
            if re.match(r"^([a-z][a-z0-9+.-]*:|//)", link, re.I):
                continue
            path, _, frag = link.partition("#")
            if link.startswith("/"):
                if rel != "404.html" or not link.startswith(base):
                    print("%s:%d: absolute link %s" % (rel, line, link))
                    bad += 1
                    continue
                path = link[len(base):].partition("#")[0]
                target = os.path.join(out, path)
            elif not path:
                target = p
            else:
                target = os.path.normpath(os.path.join(os.path.dirname(p), path.split("?")[0]))
            if os.path.isdir(target):
                target = os.path.join(target, "index.html")
            if not os.path.exists(target):
                print("%s:%d: broken %s %s" % (rel, line, kind, link))
                bad += 1
                continue
            if frag and target in pages and frag not in pages[target].ids:
                print("%s:%d: no anchor #%s in %s" % (rel, line, frag, os.path.relpath(target, out)))
                bad += 1
    print("Checked %d pages: %s" % (len(pages), "all fine" if not bad else "%d problems" % bad))
    return bad


def main():
    ap = argparse.ArgumentParser(description="Build the Kinoklip website")
    ap.add_argument("--out", default=os.path.join(REPO, "_site"))
    ap.add_argument("--base", default="/kinoklip/", help="absolute base path, used only by 404.html")
    ap.add_argument("--check", action="store_true", help="check links, images, anchors and tags after the build")
    args = ap.parse_args()
    build(args.out, args.base)
    if args.check and check(args.out, args.base):
        sys.exit(1)


if __name__ == "__main__":
    main()
