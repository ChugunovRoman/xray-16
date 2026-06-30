#include "stdafx.h"

// Stub for the disabled PFR font driver.
// pfr.c was removed from compilation (MSVC 17.4+ C99 compound literal issue),
// but ftmodule.h still declares pfr_driver_class, causing linker errors.
// This stub provides a zeroed definition so the linker is satisfied.

#include <freetype/internal/ftdrv.h>
#include <freetype/internal/ftobjs.h>

extern "C" const FT_Driver_ClassRec pfr_driver_class;
const FT_Driver_ClassRec pfr_driver_class{};
