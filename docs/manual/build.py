#!/usr/bin/env python3
"""Builds the PitchNet Reference Manual (static HTML) into ./site.

Pages are written as small HTML fragments in pages.py; this script wraps each
one in the shared layout (sidebar, breadcrumbs, prev/next, footer).
"""
import os, re, shutil, html
from pages import PAGES, NAV, VERSION

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "site")

def page_title(slug):
    for p in PAGES:
        if p["slug"] == slug:
            return p["title"]
    raise KeyError(slug)

def sidebar(current):
    out = []
    for group, items in NAV:
        out.append(f'<div class="nav-group"><div class="nav-head">{html.escape(group)}</div><ul>')
        for slug in items:
            cls = ' class="current"' if slug == current else ""
            out.append(f'<li><a href="{slug}.html"{cls}>{html.escape(page_title(slug))}</a></li>')
        out.append("</ul></div>")
    return "\n".join(out)

def figure(m):
    src, caption = m.group(1), m.group(2)
    if not os.path.exists(os.path.join(HERE, "images", src)):
        return ""  # screenshot not captured yet: leave it out
    return (f'<figure><a href="images/{src}"><img src="images/{src}" alt="{html.escape(caption)}" loading="lazy"></a>'
            f'<figcaption>{caption}</figcaption></figure>')

LAYOUT = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} - PitchNet Manual</title>
<link rel="stylesheet" href="style.css">
<link rel="icon" href="images/icon-128.png" type="image/png">
</head>
<body>
<header class="topbar">
  <button class="menu-btn" aria-label="Menu" onclick="document.body.classList.toggle('nav-open')">&#9776;</button>
  <a class="brand" href="index.html"><img src="images/icon-128.png" alt="">PitchNet <span>Manual</span></a>
  <span class="version">PitchNet {version}</span>
  <form class="search" action="search.html"><input name="q" type="search" placeholder="Search the manual" aria-label="Search the manual"></form>
</header>
<div class="wrap">
<nav class="sidebar">{sidebar}</nav>
<main>
<div class="crumbs"><a href="index.html">Manual Home</a>{crumb}</div>
<h1>{title}</h1>
{body}
<div class="pager">{prev}{next}</div>
<footer>
<p>This manual describes PitchNet {version} (standalone app and plug-in). Screenshots are from macOS; on Windows and Linux,
<kbd>Cmd</kbd> is <kbd>Ctrl</kbd> and <kbd>Option</kbd> is <kbd>Alt</kbd>.</p>
<p>PitchNet is free software by Session Loops, derived from HachiTune, and released under the
<a href="credits.html">GNU AGPLv3</a>. <a href="https://github.com/SessionLoops/PitchNet">Source on GitHub</a> &middot;
<a href="https://sessionloops.com/pitchnet#downloads">Download PitchNet</a></p>
</footer>
</main>
</div>
<script src="manual.js"></script>
</body>
</html>
"""

def build():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    shutil.copytree(os.path.join(HERE, "images"), os.path.join(OUT, "images"))
    for f in ("style.css", "manual.js", "search.html"):
        shutil.copy(os.path.join(HERE, f), OUT)
    order = [s for _, items in NAV for s in items]
    index = []
    for p in PAGES:
        body = re.sub(r'\[\[fig:([^|\]]+)\|([^\]]*)\]\]', figure, p["body"])
        i = order.index(p["slug"])
        prev = f'<a class="prev" href="{order[i-1]}.html">&larr; {html.escape(page_title(order[i-1]))}</a>' if i > 0 else "<span></span>"
        nxt = f'<a class="next" href="{order[i+1]}.html">{html.escape(page_title(order[i+1]))} &rarr;</a>' if i + 1 < len(order) else "<span></span>"
        crumb = "" if p["slug"] == "index" else f' &rsaquo; {html.escape(p["title"])}'
        out = LAYOUT.format(title=html.escape(p["title"]), version=VERSION, sidebar=sidebar(p["slug"]),
                            crumb=crumb, body=body, prev=prev, next=nxt)
        with open(os.path.join(OUT, p["slug"] + ".html"), "w") as fh:
            fh.write(out)
        text = re.sub(r"<[^>]+>", " ", body)
        index.append({"slug": p["slug"], "title": p["title"], "text": re.sub(r"\s+", " ", html.unescape(text))})
    import json
    with open(os.path.join(OUT, "search-index.js"), "w") as fh:
        fh.write("window.MANUAL_INDEX=" + json.dumps(index) + ";")
    open(os.path.join(OUT, ".nojekyll"), "w").close()
    print("built", len(PAGES), "pages ->", OUT)

if __name__ == "__main__":
    build()


def build_single(path):
    """One self-contained HTML file (images inlined, #page routing) for hosts
    that serve a single document."""
    import base64, json, mimetypes
    def inline(m):
        src = m.group(2)
        f = os.path.join(HERE, src)
        if not os.path.exists(f):
            return m.group(0)
        mime = mimetypes.guess_type(f)[0] or "image/png"
        return f'{m.group(1)}="data:{mime};base64,{base64.b64encode(open(f,"rb").read()).decode()}"'
    def links(body):
        body = re.sub(r'href="([a-z0-9-]+)\.html#([A-Za-z0-9-]+)"', r'href="#\1--\2"', body)
        body = re.sub(r'href="([a-z0-9-]+)\.html"', r'href="#\1"', body)
        return body
    sections = []
    order = [s for _, items in NAV for s in items]
    for p in PAGES:
        body = re.sub(r'\[\[fig:([^|\]]+)\|([^\]]*)\]\]', figure, p["body"])
        body = re.sub(r'id="([A-Za-z0-9-]+)"', lambda m: f'id="{p["slug"]}--{m.group(1)}"', body)
        # in-page anchors of the same page
        body = re.sub(r'href="#([A-Za-z0-9-]+)"', lambda m: f'href="#{p["slug"]}--{m.group(1)}"', body)
        body = links(body)
        i = order.index(p["slug"])
        prev = f'<a href="#{order[i-1]}">&larr; {html.escape(page_title(order[i-1]))}</a>' if i else "<span></span>"
        nxt = f'<a href="#{order[i+1]}">{html.escape(page_title(order[i+1]))} &rarr;</a>' if i + 1 < len(order) else "<span></span>"
        crumb = "" if p["slug"] == "index" else f' &rsaquo; {html.escape(p["title"])}'
        sections.append(f'<section class="pg" id="{p["slug"]}" hidden><div class="crumbs"><a href="#index">Manual Home</a>{crumb}</div>'
                        f'<h1>{html.escape(p["title"])}</h1>{body}<div class="pager">{prev}{nxt}</div></section>')
    side = links(sidebar("")).replace('href="#', 'data-p="1" href="#')
    css = open(os.path.join(HERE, "style.css")).read()
    doc = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>PitchNet Manual</title><style>{css}
.pg[hidden]{{display:none}} #sr li{{margin-bottom:8px}} #sr .snip{{display:block;color:var(--muted);font-size:14px}}</style></head>
<body>
<header class="topbar"><button class="menu-btn" aria-label="Menu" id="mb">&#9776;</button>
<a class="brand" href="#index"><img src="images/icon-128.png" alt="">PitchNet <span>Manual</span></a>
<span class="version">PitchNet {VERSION}</span>
<form class="search" id="sf"><input id="q" type="search" placeholder="Search the manual" aria-label="Search the manual"></form></header>
<div class="wrap"><nav class="sidebar">{side}</nav>
<main>{''.join(sections)}
<section class="pg" id="search" hidden><div class="crumbs"><a href="#index">Manual Home</a> &rsaquo; Search</div><h1>Search</h1><p id="ss"></p><ol id="sr"></ol></section>
<footer><p>This manual describes PitchNet {VERSION}. Screenshots are from macOS; on Windows and Linux, <kbd>Cmd</kbd> is <kbd>Ctrl</kbd> and <kbd>Option</kbd> is <kbd>Alt</kbd>.</p>
<p>PitchNet is free software by Session Loops, derived from HachiTune, released under the GNU AGPLv3.
<a href="https://github.com/SessionLoops/PitchNet">Source on GitHub</a> &middot; <a href="https://sessionloops.com/pitchnet#downloads">Download PitchNet</a></p></footer>
</main></div>
<script>{{
const show=()=>{{let h=decodeURIComponent(location.hash.slice(1))||'index';let [pg,anchor]=h.split('--');
if(!document.getElementById(pg)||!document.getElementById(pg).classList.contains('pg')){{pg='index';anchor=null}}
document.querySelectorAll('.pg').forEach(s=>s.hidden=s.id!==pg);
document.querySelectorAll('.sidebar a').forEach(a=>a.classList.toggle('current',a.getAttribute('href')==='#'+pg));
document.body.classList.remove('nav-open');
const t=anchor?document.getElementById(pg+'--'+anchor):null; if(t)t.scrollIntoView(); else window.scrollTo(0,0);}};
window.addEventListener('hashchange',show);show();
document.getElementById('mb').onclick=()=>document.body.classList.toggle('nav-open');
const idx=[...document.querySelectorAll('.pg')].filter(s=>s.id!=='search').map(s=>({{id:s.id,title:s.querySelector('h1').textContent,text:s.textContent.replace(/\\s+/g,' ')}}));
const esc=s=>s.replace(/[&<>"']/g,c=>({{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}}[c]));
document.getElementById('sf').onsubmit=e=>{{e.preventDefault();const q=document.getElementById('q').value.trim();const terms=q.toLowerCase().split(/\\s+/).filter(Boolean);
const hits=idx.map(p=>{{const t=p.text.toLowerCase();let sc=0;for(const w of terms){{const n=t.split(w).length-1;if(!n)return null;sc+=n+(p.title.toLowerCase().includes(w)?20:0)}}
const at=Math.max(0,t.indexOf(terms[0])-80);let sn=esc(p.text.substr(at,220));for(const w of terms)sn=sn.replace(new RegExp('('+w.replace(/[.*+?^${{}}()|[\\]\\\\]/g,'\\\\$&')+')','ig'),'<mark>$1</mark>');return{{p,sc,sn}}}}).filter(Boolean).sort((a,b)=>b.sc-a.sc);
document.getElementById('ss').textContent=terms.length?hits.length+' page(s) found for “'+q+'”.':'Type a word to search.';
document.getElementById('sr').innerHTML=hits.map(h=>'<li><a href="#'+h.p.id+'">'+esc(h.p.title)+'</a><span class="snip">…'+h.sn+'…</span></li>').join('');
location.hash='search';}};
}}</script></body></html>"""
    doc = re.sub(r'(src|href)="(images/[^"]+)"', inline, doc)
    with open(path, "w") as fh:
        fh.write(doc)
    print("single-file manual ->", path, os.path.getsize(path)//1024, "KB")

if __name__ == "__main__" and "--single" in __import__("sys").argv:
    build_single(os.path.join(HERE, "pitchnet-manual.html"))
