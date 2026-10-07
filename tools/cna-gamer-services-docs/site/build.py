#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render the dependency-free offline maintainer website."""

from __future__ import annotations

import argparse
import html
import json
import pathlib
import re
import shutil


FRONT = re.compile(r"\A---\n(.*?)\n---\n", re.DOTALL)
LINK = re.compile(r"\[([^]]+)\]\(([^)]+)\)")
CODE = re.compile(r"`([^`]+)`")
BOLD = re.compile(r"\*\*([^*]+)\*\*")


def slug(value):
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def inline(value):
    escaped = html.escape(value, quote=True)
    escaped = CODE.sub(lambda match: f"<code>{match.group(1)}</code>", escaped)
    escaped = BOLD.sub(lambda match: f"<strong>{match.group(1)}</strong>", escaped)
    escaped = LINK.sub(lambda match: f'<a href="{html.escape(match.group(2), quote=True)}">{match.group(1)}</a>', escaped)
    return escaped


def diagram(lines):
    exchanges = []
    participants = []
    for raw in lines:
        match = re.match(r"\s*([^:<]+?)\s*(->|<-)\s*([^:]+?):\s*(.+)\s*$", raw)
        if not match:
            continue
        left, arrow, right, label = (item.strip() for item in match.groups())
        if arrow == "<-":
            left, right = right, left
        for participant in (left, right):
            if participant not in participants:
                participants.append(participant)
        exchanges.append((left, right, label))
    width = max(680, 170 * len(participants));height = 90 + 62 * len(exchanges)
    positions = {name: 80 + index * (width - 160) / max(1, len(participants) - 1)
                 for index, name in enumerate(participants)}
    parts = [f'<svg class="sequence" viewBox="0 0 {width} {height}" role="img">']
    for name, x in positions.items():
        parts.append(f'<text x="{x}" y="24" text-anchor="middle">{html.escape(name)}</text>')
        parts.append(f'<line x1="{x}" y1="35" x2="{x}" y2="{height - 15}" class="lifeline"/>')
    for index, (left, right, label) in enumerate(exchanges):
        y = 70 + index * 62;x1, x2 = positions[left], positions[right]
        parts.append(f'<line x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" class="arrow" marker-end="url(#arrow)"/>')
        parts.append(f'<text x="{(x1+x2)/2}" y="{y-9}" text-anchor="middle">{html.escape(label)}</text>')
    parts.insert(1, '<defs><marker id="arrow" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 Z"/></marker></defs>')
    parts.append("</svg>")
    return "".join(parts)


def render_markdown(text):
    lines = text.splitlines();output = [];toc = [];index = 0
    while index < len(lines):
        line = lines[index]
        if not line.strip():index += 1;continue
        if line.startswith("```"):
            language = line[3:].strip();index += 1;block = []
            while index < len(lines) and not lines[index].startswith("```"):
                block.append(lines[index]);index += 1
            index += 1
            if language == "diagram":output.append(diagram(block))
            else:
                code = html.escape("\n".join(block))
                output.append(f'<div class="code"><button class="copy" type="button">Copy</button><pre><code class="language-{slug(language)}">{code}</code></pre></div>')
            continue
        heading = re.match(r"^(#{1,4})\s+(.+)$", line)
        if heading:
            level = len(heading.group(1));title = heading.group(2);anchor = slug(title)
            if level in (2, 3):toc.append((level, title, anchor))
            output.append(f'<h{level} id="{anchor}">{inline(title)}</h{level}>');index += 1;continue
        if line.startswith("|") and index + 1 < len(lines) and re.match(r"^\|?\s*:?-+", lines[index + 1]):
            headers = [item.strip() for item in line.strip("|").split("|")];index += 2;rows = []
            while index < len(lines) and lines[index].startswith("|"):
                rows.append([item.strip() for item in lines[index].strip("|").split("|")]);index += 1
            output.append("<div class=table-wrap><table><thead><tr>" + "".join(f"<th>{inline(item)}</th>" for item in headers) +
                          "</tr></thead><tbody>" + "".join("<tr>" + "".join(f"<td>{inline(item)}</td>" for item in row) + "</tr>" for row in rows) + "</tbody></table></div>")
            continue
        if re.match(r"^[-*]\s+", line):
            items = []
            while index < len(lines) and re.match(r"^[-*]\s+", lines[index]):
                items.append(re.sub(r"^[-*]\s+", "", lines[index]));index += 1
            output.append("<ul>" + "".join(f"<li>{inline(item)}</li>" for item in items) + "</ul>");continue
        if re.match(r"^\d+\.\s+", line):
            items = []
            while index < len(lines) and re.match(r"^\d+\.\s+", lines[index]):
                items.append(re.sub(r"^\d+\.\s+", "", lines[index]));index += 1
            output.append("<ol>" + "".join(f"<li>{inline(item)}</li>" for item in items) + "</ol>");continue
        if line.startswith("> "):
            output.append(f"<blockquote>{inline(line[2:])}</blockquote>");index += 1;continue
        paragraph = [line];index += 1
        while index < len(lines) and lines[index].strip() and not re.match(r"^(#{1,4}\s|```|[-*]\s|\d+\.\s|>\s|\|)", lines[index]):
            paragraph.append(lines[index]);index += 1
        output.append(f"<p>{inline(' '.join(item.strip() for item in paragraph))}</p>")
    toc_html = "<ol>" + "".join(f'<li class="level-{level}"><a href="#{anchor}">{html.escape(title)}</a></li>' for level, title, anchor in toc) + "</ol>"
    return "\n".join(output), toc_html


def load_pages(source):
    pages = []
    for path in sorted(source.glob("*.md")):
        text = path.read_text(encoding="utf-8");match = FRONT.match(text)
        if not match:raise SystemExit(f"{path}: missing front matter")
        metadata = {}
        for line in match.group(1).splitlines():
            key, value = line.split(":", 1);metadata[key.strip()] = value.strip()
        metadata.update(source=path, filename=path.stem + ".html", body=text[match.end():])
        metadata["order"] = int(metadata["order"]);pages.append(metadata)
    pages.sort(key=lambda page: page["order"])
    if not pages:raise SystemExit("no site pages")
    return pages


def template(page, pages, content, toc, previous, following):
    navigation = "".join(f'<a class="{"active" if item is page else ""}" href="{item["filename"]}"><span>{item["order"]:02}</span>{html.escape(item["title"])}</a>' for item in pages)
    pager = "<div class=pager>" + (f'<a href="{previous["filename"]}">← {html.escape(previous["title"])}</a>' if previous else "<span></span>") + (f'<a href="{following["filename"]}">{html.escape(following["title"])} →</a>' if following else "") + "</div>"
    return f"""<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><meta name=description content="{html.escape(page['summary'], quote=True)}"><title>{html.escape(page['title'])} — CNA Gamer Services</title><link rel=stylesheet href=assets/site.css><script defer src=assets/search-index.js></script><script defer src=assets/site.js></script></head><body><a class=skip href=#content>Skip to content</a><header><button id=nav-toggle aria-label="Toggle navigation">☰</button><a class=brand href=index.html>CNA Gamer Services<br><small>maintainer handbook</small></a><label class=search>Search <input id=search-input autocomplete=off placeholder="operation, error, concept…"></label></header><div class=layout><nav id=site-nav>{navigation}</nav><main id=content><p class=eyebrow>Chapter {page['order']:02}</p><h1>{html.escape(page['title'])}</h1><p class=summary>{html.escape(page['summary'])}</p><aside class=toc><strong>On this page</strong>{toc}</aside>{content}{pager}</main><aside id=search-results aria-live=polite></aside></div><footer>Generated from repository-owned Markdown. Normative wire contracts remain <code>protocol/v1.md</code> and <code>protocol/relay-v1.md</code>.</footer></body></html>"""


CSS = """
:root{--ink:#14202b;--muted:#576574;--nav:#102a43;--accent:#0b7285;--paper:#fff;--wash:#f1f5f8;font:16px/1.62 system-ui,sans-serif;color:var(--ink);background:var(--wash)}*{box-sizing:border-box}body{margin:0}a{color:#086f83}header{position:sticky;top:0;z-index:5;height:74px;background:var(--nav);color:white;display:flex;align-items:center;gap:1.5rem;padding:.65rem 1.2rem}.brand{color:white;text-decoration:none;font-weight:700;line-height:1.15}.brand small{font-weight:400;color:#b9d6e2}.search{margin-left:auto}.search input{margin-left:.4rem;padding:.5rem;width:min(34vw,28rem)}#nav-toggle{display:none}.layout{display:grid;grid-template-columns:18rem minmax(0,56rem) minmax(0,18rem);max-width:96rem;margin:auto}#site-nav{position:sticky;top:74px;height:calc(100vh - 74px);overflow:auto;background:#e8eef3;padding:.75rem}#site-nav a{display:flex;gap:.55rem;padding:.38rem .5rem;text-decoration:none;color:#294052;border-radius:.25rem}#site-nav a span{color:#718392}#site-nav a.active{background:white;color:#064f5c;font-weight:650}main{background:var(--paper);padding:2.2rem 3rem;min-height:calc(100vh - 74px)}h1{font-size:2.35rem;line-height:1.1;margin:.2rem 0}h2{margin-top:2.5rem;border-bottom:1px solid #d8e1e8;padding-bottom:.25rem}h3{margin-top:1.7rem}.summary{font-size:1.18rem;color:var(--muted)}.eyebrow{text-transform:uppercase;letter-spacing:.12em;color:var(--accent);font-size:.78rem;font-weight:700}.toc{background:#edf8fa;border-left:4px solid var(--accent);padding:.8rem 1.1rem}.toc ol{columns:2}.toc li{margin:.15rem}.toc .level-3{margin-left:1rem;font-size:.92rem}code{background:#edf1f4;padding:.08rem .28rem;border-radius:.2rem}.code{position:relative}.code pre{overflow:auto;background:#102a43;color:#f5fbff;padding:1rem;border-radius:.35rem}.code code{background:none;padding:0}.copy{position:absolute;right:.45rem;top:.45rem}.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%}th,td{border:1px solid #ccd8e0;padding:.45rem;text-align:left;vertical-align:top}th{background:#eaf1f5}blockquote{border-left:4px solid #f0a202;margin:1rem 0;padding:.5rem 1rem;background:#fff7df}.sequence{width:100%;min-width:580px;border:1px solid #d8e1e8;background:#fbfdff}.sequence text{font:13px system-ui;fill:#14202b}.sequence .lifeline{stroke:#9aabb8;stroke-dasharray:5 5}.sequence .arrow{stroke:#0b7285;stroke-width:2}.sequence marker path{fill:#0b7285}.pager{display:flex;justify-content:space-between;border-top:1px solid #ccd8e0;margin-top:3rem;padding-top:1rem}#search-results{padding:1rem;position:sticky;top:74px;height:calc(100vh - 74px);overflow:auto}#search-results:empty:before{content:"Search spans every chapter";color:var(--muted)}#search-results a{display:block;background:white;padding:.65rem;margin-bottom:.5rem;text-decoration:none}.skip{position:absolute;left:-9999px}.skip:focus{left:1rem;top:1rem;z-index:10;background:white;padding:.5rem}footer{background:var(--nav);color:#d9e7ef;padding:1rem;text-align:center}@media(max-width:1100px){.layout{grid-template-columns:16rem minmax(0,1fr)}#search-results{display:none}}@media(max-width:760px){header{height:auto;flex-wrap:wrap}.search{order:3;width:100%}.search input{width:75%}#nav-toggle{display:block}.layout{display:block}#site-nav{display:none;position:fixed;z-index:4;top:112px;width:85%;height:calc(100vh - 112px)}#site-nav.open{display:block}main{padding:1.5rem}.toc ol{columns:1}.sequence{overflow:auto}}
"""


JS = """
document.querySelectorAll('.copy').forEach(button=>button.addEventListener('click',()=>{navigator.clipboard.writeText(button.nextElementSibling.innerText);button.textContent='Copied'}));
const nav=document.querySelector('#site-nav'),toggle=document.querySelector('#nav-toggle');toggle.addEventListener('click',()=>nav.classList.toggle('open'));
const input=document.querySelector('#search-input'),results=document.querySelector('#search-results');input.addEventListener('input',()=>{const words=input.value.toLowerCase().trim().split(/\\s+/).filter(Boolean);results.innerHTML='';if(!words.length)return;SEARCH_INDEX.map(page=>({page,score:words.reduce((n,w)=>n+(page.text.includes(w)?1:0),0)})).filter(x=>x.score).sort((a,b)=>b.score-a.score||a.page.order-b.page.order).slice(0,12).forEach(({page})=>{const a=document.createElement('a');a.href=page.url;a.textContent=page.title;results.append(a)})});
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    options = parser.parse_args();source = pathlib.Path(__file__).with_name("pages");pages = load_pages(source)
    output = options.output.resolve()
    if output == pathlib.Path("/") or output == pathlib.Path.home():raise SystemExit("refusing broad output path")
    output.mkdir(parents=True, exist_ok=True);assets = output / "assets";assets.mkdir(exist_ok=True)
    search = []
    for index, page in enumerate(pages):
        content, toc = render_markdown(page["body"]);previous = pages[index-1] if index else None
        following = pages[index+1] if index+1 < len(pages) else None
        (output / page["filename"]).write_text(template(page, pages, content, toc, previous, following), encoding="utf-8")
        plain = re.sub(r"\s+", " ", re.sub(r"[`*#>|\[\]()]", " ", page["body"])).lower()
        search.append({"title": page["title"], "url": page["filename"], "order": page["order"], "text": plain})
    shutil.copyfile(output / pages[0]["filename"], output / "index.html")
    (assets / "site.css").write_text(CSS, encoding="utf-8")
    (assets / "site.js").write_text(JS, encoding="utf-8")
    (assets / "search-index.js").write_text("const SEARCH_INDEX=" + json.dumps(search, separators=(",", ":")) + ";\n", encoding="utf-8")
    print(f"Built {len(pages)} documentation pages in {output}")


if __name__ == "__main__":main()
