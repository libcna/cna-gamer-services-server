---
title: Avatars and catalog packs
summary: Compact account descriptions, versioned catalogs, installable packs, projection behavior and validation boundaries.
order: 10
---

## Stored representation

An account stores one fixed-size CNA avatar description, revision and update time—not meshes and
textures copied per player. Item identifiers in the description refer to an immutable versioned
catalog. CNA renders installed catalog geometry locally.

The administrator imports a complete catalog directory containing `catalog.json` and every named
asset. The importer validates bounded names, hashes, GLB structure, item slots, compatibility with
older versions and semantic constraints before one transaction publishes it. Catalog versions only
grow; changing the meaning of an existing item would make old descriptions render differently.

## Negotiation and projection

```diagram
Game -> Server: avatars.get(installed catalog versions)
Server -> SQLite: read account description/version
Server -> Game: stored avatar if catalog available
Server -> Game: marked projection if catalog unavailable
Game -> Server: avatars.catalogPack(new version)
Server -> Game: manifest and content hashes
Game -> File route: fetch missing hashes
Game -> Game cache: validate whole pack, activate atomically
```

A client able to install the referenced version downloads one pack and then receives the stored
description. A client that declines or cannot install gets a projection onto a catalog it already
has. Projection is explicit compatibility behavior, not silent mutation of the account avatar.

## Administration

`avatar <username> random [male|female]` creates a valid description from installed items. `clear`
removes it. `set` consumes exactly the expected lowercase description hex on stdin. The admin web
shows catalog/item/asset counts and account revision metadata; it deliberately does not dump binary
descriptions into pages or logs.

## Validation and failures

Treat avatar parsing as an untrusted binary boundary. Bounds precede allocation and reference use.
Asset names cannot contain paths, model sizes are capped, hashes must match and a catalog import is
all-or-nothing. `AvatarValidationTests` can optionally run CNA's real catalogs as golden fixtures;
absence of those external fixtures is a skip, not validation.

For a missing avatar, distinguish: account has none; client lacks the catalog; catalog pack was
declined/corrupt; projection occurred; or file authorization failed. The manifest hash and catalog
version are better correlation values than a gamertag in operational logs.
