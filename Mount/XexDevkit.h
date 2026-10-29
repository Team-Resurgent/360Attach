// On a devkit, resign the disc's root default.xex in memory (XexTool -m d:
// rewrap the image key and replace the debug signature). Only the header
// sectors that change are kept; the rest of the file is still read from the image.
#ifndef __XEXDEVKIT_H
#define __XEXDEVKIT_H

// True when XeKeysGetConsoleType reports a development kit.
BOOL XexDevkitIsConsoleDevkit( VOID );

// Drops any patched sectors from the previous mount.
VOID XexDevkitClear( VOID );

// Reads the XEX at CCI-stream sector imageLba and, when this is a devkit,
// builds the patched header. partitionSector is that same file in the
// partition-relative units reads use. fileBytes is the XDVDFS size.
// An image that is already debug-signed, or a manufacturing image, is left
// unchanged. Failure means a devkit would be handed a retail signature.
NTSTATUS XexDevkitPatch( DWORD imageLba, DWORD partitionSector, DWORD fileBytes );

// Copies patched header bytes over a partition-relative read of length bytes
// at sector. No effect when no patch is active.
VOID XexDevkitSplice( DWORD sector, DWORD length, BYTE * dest );

#endif // __XEXDEVKIT_H
