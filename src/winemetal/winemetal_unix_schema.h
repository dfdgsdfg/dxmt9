#pragma once

/*
 * Native-service unix bridge schema placeholder.
 *
 * Exact-parity upstream ownership wants this header to describe only the
 * native-service thunk surface that winemetal.so publishes. The current bridge
 * generator still needs dxmt9c_* declarations during the transition, but those
 * are now injected from a separately extracted schema target instead of by
 * including the public device_c.h ABI here directly.
 */
