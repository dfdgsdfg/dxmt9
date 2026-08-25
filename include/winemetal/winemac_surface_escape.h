/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Declaration-only compatibility surface derived from Wine MR !11058,
 * head commit 07bb09bd2d3974ec035ec0e49fa5bc7e85a3ab41. The implementation
 * lives in winemac.drv; dxmt9 carries only the escape values and POD layout.
 *
 * Copyright 1993-2026 the Wine project authors
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum macdrv_surface_escape_code {
  MACDRV_ESCAPE_GET_SURFACE = 6790,
  MACDRV_ESCAPE_RELEASE_SURFACE = 6791,
};

typedef struct macdrv_escape_surface {
  uint64_t surface;
  uint64_t layer;
} macdrv_escape_surface;

#ifdef __cplusplus
}

static_assert(sizeof(macdrv_escape_surface) == 16);
static_assert(alignof(macdrv_escape_surface) == alignof(uint64_t));
#endif
