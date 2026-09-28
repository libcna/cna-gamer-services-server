// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Store.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace CnaService {
/** @brief Length of every avatar description. */
inline constexpr std::size_t AvatarDescriptionSize=1021;
/** @brief IEEE CRC-32. @param bytes Input. @return Checksum. */
std::uint32_t crc32(std::string_view bytes);
/** @brief Accepts only a CNA v1 description whose catalog version and items were imported here.
 * @param db Database. @param description Raw bytes. @throws Error INVALID_ARGUMENT otherwise. */
void validateAvatarDescription(sqlite3* db,std::string_view description);
/** @brief Imports an avatar catalog: validates the manifest, every file's hash, size and type, and
 * that no earlier item disappears or changes slot. Re-importing the identical catalog is a no-op.
 * @param store Store. @param manifest catalog.json text. @param files Asset bytes by name.
 * @return Imported catalog version. */
int importAvatarCatalog(Store& store,std::string_view manifest,const std::map<std::string,std::string>& files);
/** @brief Builds a random description from the newest imported catalog (administration).
 * @param db Database. @param bodyType 0, 1, or empty for either. @return Bytes. */
std::string randomAvatarDescription(sqlite3* db,std::optional<int> bodyType);
/** @brief Sets or clears an account's avatar (administration). @param store Store.
 * @param username Account. @param description Bytes, or empty to clear. */
void setAvatar(Store& store,const std::string& username,const std::optional<std::string>& description);
/** @brief Stores the caller's own avatar and bumps its revision. @param db Database.
 * @param userId Account. @param description Validated bytes. @param now Timestamp. @return Revision. */
long long storeAvatar(sqlite3* db,const std::string& userId,std::string_view description,long long now);
}
