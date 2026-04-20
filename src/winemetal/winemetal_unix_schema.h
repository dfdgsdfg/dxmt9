#pragma once

/*
 * Native-service unix bridge schema (placeholder).
 *
 * winemetal.so no longer hosts the generated device_c bridge; the only
 * thunk surface it publishes is the WMT wrapper API implemented in
 * winemetal_private_api.mm and declared in winemetal.h. This header remains
 * as a compatibility stub in case downstream bridge-generation targets still
 * reference it; it intentionally declares nothing.
 */
