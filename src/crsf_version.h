/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file crsf_version.h
 * @brief Which version of the library this is.
 *
 * The root VERSION file is the authority and this header mirrors it; a lint job
 * fails if the two disagree. The header exists because it is the part that
 * survives vendoring: a platform port copies `include/` and `src/` into itself,
 * and a bug report from that port needs to be able to say which core it was
 * built against.
 */

#ifndef CRSF_VERSION_H
#define CRSF_VERSION_H

/** Major version: incremented when the public API breaks. */
#define CRSF_VERSION_MAJOR 1
/** Minor version: incremented when the API gains something, compatibly. */
#define CRSF_VERSION_MINOR 1
/** Patch version: incremented for fixes that change no declaration. */
#define CRSF_VERSION_PATCH 0

/** The version as a string, matching the root VERSION file exactly. */
#define CRSF_VERSION_STRING "1.1.0"

/**
 * @brief The version as one comparable integer.
 *
 * `#if CRSF_VERSION >= CRSF_VERSION_MAKE(1, 2, 0)` is the intended use.
 */
#define CRSF_VERSION \
    CRSF_VERSION_MAKE(CRSF_VERSION_MAJOR, CRSF_VERSION_MINOR, CRSF_VERSION_PATCH)

/**
 * @brief Build a comparable version number.
 * @param maj Major.
 * @param min Minor.
 * @param pat Patch.
 */
#define CRSF_VERSION_MAKE(maj, min, pat) ((maj) * 10000 + (min) * 100 + (pat))

#endif /* CRSF_VERSION_H */
