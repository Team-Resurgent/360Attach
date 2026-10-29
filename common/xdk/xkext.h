#ifndef __XKEXT_H
#define __XKEXT_H

// The undocumented kernel and XAM exports the launcher and driver use. Only the
// declarations those two need are here; see the .def files for the import libraries.
#pragma warning(push)
#pragma warning(disable:4200 4201 4214)
#include "exttypes.h"
#include "_kernelext.h"
#include "_xamext.h"
#include "xapiext.h"
#pragma warning(pop)

#endif // __XKEXT_H
