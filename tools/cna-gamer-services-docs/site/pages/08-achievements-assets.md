---
title: Achievements, assets and hash-addressed files
summary: Catalog versus earned state, immutable content validation, authorization, chunk reads and raw file delivery.
order: 8
---

## Achievement model

The administrator defines a title's catalog: stable key, display name, description, how-to text,
score, display flag and optional picture hash. A player's earned row names that catalog key and a
timestamp. Definition and progress are deliberately separate: importing a definition does not
award it, and an award cannot invent metadata.

`achievements.award` is durable and idempotent under request replay. Gamerscore derives from earned
definitions rather than a separately mutable total. A reset is administrative, title-scoped and
destructive; the web UI requires typing the title ID.

## Asset validation and ownership

Assets are immutable blobs addressed by lowercase SHA-256. Import accepts bounded PNG or GLB. PNG
checks include signature, IHDR placement, dimensions and size; GLB checks magic, version and stated
length. Avatar import performs deeper catalog/model validation described in
[the avatar chapter](10-avatars.html).

`title_assets` grants a title access to a hash. Account picture and avatar catalog references add
specific authorized paths. A client cannot convert a hash into a filesystem path: the file route
accepts exactly 64 lowercase hexadecimal characters and reads the database blob.

## Two retrieval forms

`assets.read` returns a bounded range encoded in a control response and is useful for small pieces.
`GET /cna/v1/files/<sha256>` returns raw bytes with a bearer access token and `X-CNA-Game`. The raw
route avoids hex/base64 expansion for catalog packs. Both enforce title/account authorization and
an hourly per-account download budget.

Content hashes allow safe caches: a byte mismatch means corruption, and the same hash never changes
meaning. Clients should download to a temporary file, validate, then atomically publish rather than
trusting a partial cache entry.

## Operator workflow

Import the asset first, record its printed hash, then reference that hash from an achievement or
assign it as a picture. A `NOT_FOUND` during assignment usually means the hash was not imported for
that title, not that the filesystem path is wrong. Database size metrics include stored assets;
the WAL is measured separately by the host.
