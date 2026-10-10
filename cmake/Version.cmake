# SPDX-License-Identifier: MIT
# =====================================================================================
# cna-gamer-services-server release identity -> the generated header CnaService/Version.hpp.
#
# The single source of truth is project(cna_gamer_services_server VERSION ...) plus
# CNA_GAMER_SERVICES_VERSION_PRERELEASE in the root CMakeLists.txt; this file only renders that
# decision into a header. Nothing else in the tree may hard-code the number -- docs/releasing.md
# lists the hand-maintained copies (CHANGELOG.md, README.md) that a release bump must update.
#
# The header lands in its own generated/include root, published by the cna_service target, so the
# server, the admin CLI and the tests include <CnaService/Version.hpp> like any public header. The
# migration headers in generated/ stay private to cna_service.
# =====================================================================================

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/CnaService/Version.hpp"
    @ONLY)
