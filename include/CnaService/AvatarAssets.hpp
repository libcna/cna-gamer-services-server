// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Protocol.hpp"
#include <array>
#include <cstddef>
#include <string_view>

namespace CnaService {
/** @brief Joints in the CNA avatar rig (XNA's avatar skeleton). */
inline constexpr std::size_t AvatarJointCount=71;

/** @brief What an accepted avatar GLB contains, for catalog-level checks. */
struct AvatarGlbSummary {
    /** @brief Bind translation of every joint, by rig slot. */
    std::array<std::array<float,3>,AvatarJointCount> bind{};
    /** @brief Mesh primitives. */
    std::size_t primitives=0;
    /** @brief Animations. */
    std::size_t animations=0;
};

/** @brief Validates a GLB against the avatar contract CNA clients enforce: one embedded buffer, the
 * 71-joint rig in its exact topology with identity bind rotations, skinned triangle primitives with
 * only POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0, in-range indices and weights, embedded PNG
 * base-colour textures, CNA material extras, cubic-spline joint animations and bounded sizes.
 * @param bytes GLB. @return Summary. @throws Error INVALID_ARGUMENT otherwise. */
AvatarGlbSummary validateAvatarGlb(std::string_view bytes);

/** @brief Validates an avatar face atlas PNG and its layout (every XNA eye, eyebrow and mouth
 * state, tiles in range). @param png Atlas. @param layout Manifest face layout.
 * @throws Error INVALID_ARGUMENT otherwise. */
void validateFaceAtlas(std::string_view png,const Json& layout);

/** @brief Validates a manifest's faceControls (bounded deformers per body type).
 * @param controls Value of "faceControls". @throws Error INVALID_ARGUMENT otherwise. */
void validateFaceControls(const Json& controls);
}
