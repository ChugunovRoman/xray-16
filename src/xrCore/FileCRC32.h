#pragma once

XRCORE_API void getFileCrc32(IReader* F, pcstr filePath, u32& outCrc, bool parseIncludes = true); // sets the value of outCrc
XRCORE_API void addFileCrc32(IReader* F, pcstr filePath, u32& outCrc, bool parseIncludes = true); // just adds to outCrc
// 50z2: as getFileCrc32 with includes, but the CRC of each #include is kept by its full path for the process (shader
// creation reads the same includes for every shader). resetFileCrc32IncludeMemo drops it: whenever the files may change.
XRCORE_API void getFileCrc32MemoIncludes(IReader* F, pcstr filePath, u32& outCrc); // sets the value of outCrc
XRCORE_API void resetFileCrc32IncludeMemo();
