/* The Xbox 360 XDK has no <intrin.h>; LZ4 includes it unconditionally under
   MSVC, and with LZ4_FORCE_SW_BITCOUNT it needs nothing from it. */
#pragma once
#include <ppcintrinsics.h>
