#pragma once

/*
 * Transitional unix bridge schema.
 *
 * Exact-parity upstream ownership wants device_c.h to become a pure PE/provider
 * ABI and for winemetal.so to publish only native-service entries. The current
 * tree is not there yet, but the generator now consumes this internal schema
 * path instead of reading device_c.h directly so the bridge surface can be
 * narrowed in staged landings without rebasing callers again.
 */

#include "../../include/dxmt9/device_c.h"
