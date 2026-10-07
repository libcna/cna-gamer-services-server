# CNA Gamer Services documentation tools

This directory owns the dependency-light offline technical website and its drift checker:

```sh
python3 tools/cna-gamer-services-docs/check.py . --strict
```

It validates local Markdown links and fenced JSON; numbered migrations, `user_version` and
`Store::SchemaVersion`; required campaign artifacts; every documented admin command/exported
metric; site topic coverage; and a complete generated-site build with local HTML links/assets. It
runs as part of `tools/qualify.sh full`.

The website is 26 interconnected chapters sourced from plain Markdown in `site/pages`. Build and
serve it without dependencies:

```sh
python3 tools/cna-gamer-services-docs/site/build.py --output /tmp/cna-docs
python3 -m http.server --directory /tmp/cna-docs 8000
```

See [`site/README.md`](site/README.md) for its intentionally small Markdown/diagram conventions.
