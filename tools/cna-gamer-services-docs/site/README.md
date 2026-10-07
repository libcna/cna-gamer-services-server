# Offline maintainer website

The source pages are plain Markdown in `pages/`. `build.py` is a small standard-library renderer;
there is no Node, package lock, CDN or generated source dependency.

```sh
python3 tools/cna-gamer-services-docs/site/build.py --output /tmp/cna-gamer-services-docs
python3 -m http.server --directory /tmp/cna-gamer-services-docs 8000
```

Open `http://127.0.0.1:8000/`. The generated site has persistent navigation, page contents,
previous/next links, keyboard-focusable search, copy buttons for examples, and inline SVG sequence
diagrams. Search data and all styling/scripts are local, so it remains useful without Internet
access. Running from a local HTTP server is recommended; the pages themselves do not contact one.

Page front matter contains `title`, `summary` and numeric `order`. The deliberately small Markdown
subset supports headings, paragraphs, links, lists, tables, block quotes, fenced code and fenced
`diagram` sequence descriptions. A diagram line is `Participant -> Other: message`; use `<-` for
the reverse direction. Keep normative values linked conceptually to `protocol/v1.md` and rely on
the repository documentation checker to catch internal site links, coverage, schema and exported
metric drift.

Generated output is disposable and should not be committed. The durable inputs are these pages,
the renderer and the protocol/tests they explain.
