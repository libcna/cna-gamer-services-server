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
/** @brief IEEE CRC-32. @param bytes Input. @return Checksum. */
std::uint32_t crc32(std::string_view bytes);
/** @brief Accepts only a CNA format 1 or 2 description whose catalog version, items and facial hair
 * were imported here (format 2 must use facial hair or a face shape).
 * @param db Database. @param description Raw bytes. @throws Error INVALID_ARGUMENT otherwise. */
void validateAvatarDescription(sqlite3* db,std::string_view description);
/** @brief The format 1 copy of a description for clients that read only format 1: format 2's facial
 * hair and face shape dropped, everything else kept. @param description Stored bytes. @return Bytes. */
std::string formatOneDescription(std::string_view description);
/** @brief Imports an avatar catalog: validates the manifest, every file's hash and size, every model
 * against the CNA avatar contract (AvatarAssets.hpp), that every item fits its body, the face atlas
 * layout and face controls, and that no earlier item disappears or changes slot. Re-importing the
 * identical catalog is a no-op.
 * @param store Store. @param manifest catalog.json text. @param files Asset bytes by name.
 * @return Imported catalog version. */
int importAvatarCatalog(Store& store,std::string_view manifest,const std::map<std::string,std::string>& files);
/** @brief Builds a random description from the newest imported catalog (administration).
 * @param db Database. @param bodyType 0, 1, or empty for either. @return Bytes. */
std::string randomAvatarDescription(sqlite3* db,std::optional<int> bodyType);
/** @brief Sets or clears an account's avatar (administration). @param store Store.
 * @param username Account. @param description Bytes, or empty to clear. */
void setAvatar(Store& store,const std::string& username,const std::optional<std::string>& description);
/** @brief What the service derives from one imported (immutable) catalog. */
struct CatalogInfo {
    /** @brief Catalog version. */
    long long version=0;
    /** @brief Stored canonical manifest text. */
    std::string manifest;
    /** @brief Its SHA-256: the pack's identity, and how the manifest is downloaded. */
    std::string manifestSha256;
    /** @brief Catalog contract level a client needs (manifest "reader", 1 when absent). */
    int reader=1;
    /** @brief Description formats that may name it: 1, and 2 when it has face controls or feature items. */
    std::vector<int> formats;
    /** @brief Sum of the sizes of the files the manifest lists. */
    long long totalBytes=0;
    /** @brief Item id to slot (0-5 wardrobe, 6 facial hair). */
    std::map<long long,int> items;
};
/** @brief Reads one imported catalog's facts. @param db Database. @param version Version.
 * @return Facts. @throws Error NOT_FOUND. */
CatalogInfo catalogInfo(sqlite3* db,long long version);
/** @brief The pack descriptor avatars.catalogPack returns. @param info Catalog. @return Descriptor. */
Json catalogPack(const CatalogInfo& info);
/** @brief Projects a stored description onto a catalog a client has: the same body, height, build,
 * colours and face; every item id the target catalog has in the same slot is kept (catalog ids keep
 * their item across versions), any other is its slot's default (optional slots empty); facial hair
 * and face shape only where the target has them and the client reads format 2. The stored avatar is
 * not changed. @param description Stored bytes. @param target Catalog. @param formatTwo Client reads
 * format 2. @return A valid description naming the target catalog. */
std::string projectAvatarDescription(std::string_view description,const CatalogInfo& target,bool formatTwo);
/** @brief Stores the caller's own avatar and bumps its revision. @param db Database.
 * @param userId Account. @param description Validated bytes. @param now Timestamp. @return Revision. */
long long storeAvatar(sqlite3* db,const std::string& userId,std::string_view description,long long now);
}
