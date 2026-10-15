// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Store.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CnaService {
/** @brief Length of every avatar description. */
inline constexpr std::size_t AvatarDescriptionSize=1021;
/** @brief The baseline avatar catalog, CNA's catalog v3: the oldest catalog version the service
 * accepts. Versions below it were CNA development catalogs; no catalog, pack or description naming
 * one is ever accepted. Later versions are future immutable catalogs that grow from it. */
inline constexpr long long BaseCatalogVersion=3;
/** @brief The highest catalog version a description can encode (16 bits). */
inline constexpr long long MaximumCatalogVersion=65535;
/** @brief Whether a catalog version can be supported at all: the baseline or a later version.
 * @param version Catalog version. @return False below the baseline or past 16 bits. */
constexpr bool supportedCatalogVersion(long long version)
{
    return version>=BaseCatalogVersion&&version<=MaximumCatalogVersion;
}
/** @brief IEEE CRC-32. @param bytes Input. @return Checksum. */
std::uint32_t crc32(std::string_view bytes);
/** @brief The newest CNA description format the service reads, stores and serves: format 3. */
inline constexpr int NewestDescriptionFormat=3;
/** @brief Largest catalog manifest (catalog.json) accepted, in bytes. */
inline constexpr std::size_t MaximumManifestBytes=1u<<20;
/** @brief Parses catalog.json text: at most MaximumManifestBytes, nesting 16, 4096 array items and
 * 256 distinct keys per object. @param text Manifest. @return JSON.
 * @throws Error LIMIT_EXCEEDED or MALFORMED_MESSAGE otherwise. */
Json parseAvatarManifest(std::string_view text);
/** @brief Accepts only a canonical CNA description (format 1, 2 or 3) whose catalog version is
 * supported and was imported here: every item, feature item and face style it names is that
 * catalog's in the right slot, accessory sides are in range, and its format is exactly the lowest
 * one that can say everything it says (the fields it uses and the formats of the items it names).
 * @param db Database. @param description Raw bytes. @throws Error INVALID_ARGUMENT otherwise. */
void validateAvatarDescription(sqlite3* db,std::string_view description);
/** @brief Imports an avatar catalog: validates the manifest, a supported catalog version (the
 * baseline first, a later version only once the baseline is here), every file's hash and size, every
 * model against the CNA avatar contract (AvatarAssets.hpp), that every item fits its body, the face
 * atlas layout, face styles, face controls, anchors, head surface, outfits and presets, and that no
 * earlier item disappears or changes slot. Re-importing the identical catalog is a no-op; a higher
 * "revision" of the newest imported version replaces it when every item of the stored revision keeps
 * its id, slot and description format.
 * @param store Store. @param manifest catalog.json text. @param files Asset bytes by name.
 * @return Imported catalog version. */
int importAvatarCatalog(Store& store,std::string_view manifest,const std::map<std::string,std::string>& files);
/** @brief Builds a random canonical description from the newest imported catalog (administration).
 * @param db Database. @param bodyType 0, 1, or empty for either. @return Bytes. */
std::string randomAvatarDescription(sqlite3* db,std::optional<int> bodyType);
/** @brief Sets or clears an account's avatar (administration). @param store Store.
 * @param username Account. @param description Bytes, or empty to clear. */
void setAvatar(Store& store,const std::string& username,const std::optional<std::string>& description);
/** @brief One item of an imported catalog. */
struct CatalogItemInfo {
    /** @brief Slot: 0-5 wardrobe, 6 facial hair, 7 ears, 8-11 earrings, wristwear, gloves, rings,
     * 12-16 eyebrows, eye shadow, eyeliner, blush, lips. */
    int slot=0;
    /** @brief The lowest description format that can name it (1-3). */
    int format=1;
};
/** @brief What the service derives from one imported catalog revision. */
struct CatalogInfo {
    /** @brief Catalog version. */
    long long version=0;
    /** @brief Revision of that version (manifest "revision", 1 when absent). */
    long long revision=1;
    /** @brief Stored canonical manifest text. */
    std::string manifest;
    /** @brief Its SHA-256: the pack's identity, and how the manifest is downloaded. */
    std::string manifestSha256;
    /** @brief Catalog contract level a client needs (manifest "reader", 1 when absent). */
    int reader=1;
    /** @brief Description formats that may name it: 1; 2 with face controls or feature items; 3
     * with items only format 3 can name or face styles. */
    std::vector<int> formats;
    /** @brief Sum of the sizes of the files the manifest lists. */
    long long totalBytes=0;
    /** @brief Items, feature items and face styles by id. */
    std::map<long long,CatalogItemInfo> items;
};
/** @brief Reads one imported catalog's facts. @param db Database. @param version Version.
 * @return Facts. @throws Error NOT_FOUND, always for a version below the baseline. */
CatalogInfo catalogInfo(sqlite3* db,long long version);
/** @brief The pack descriptor avatars.catalogPack returns. @param info Catalog. @return Descriptor. */
Json catalogPack(const CatalogInfo& info);
/** @brief The copy of a stored description a client that reads description formats up to `format`
 * is given on a catalog it has: the same body, height, build, colours and face; every id the target
 * catalog has in the same slot, and that the format can name, kept (catalog ids keep their item
 * across versions and revisions); any other is its slot's lowest such id (optional slots and kinds
 * emptied); facial hair and face shape only from format 2, eyebrows, ears, accessories, makeup,
 * extra colours, proportions and the second face block only from format 3, each where the target
 * has them. The result is canonical (its lowest format). The stored avatar is not changed; a
 * description the client can already read on its own catalog comes back unchanged.
 * @param description Stored bytes. @param target Catalog. @param format Highest format the client
 * reads. @return A valid description naming the target catalog. */
std::string projectAvatarDescription(std::string_view description,const CatalogInfo& target,int format);
/** @brief Stores the caller's own avatar and bumps its revision. @param db Database.
 * @param userId Account. @param description Validated bytes. @param now Timestamp. @return Revision. */
long long storeAvatar(sqlite3* db,const std::string& userId,std::string_view description,long long now);
}
