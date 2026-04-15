#pragma once

#include "dxmt9/wineunixlib.h"

#ifndef WINE_UNIX_LIB
#ifndef WINE_UNIX_CALL
#define WINE_UNIX_CALL(code, args) __wine_unix_call(__wine_unixlib_handle, code, args)
#endif
#endif
