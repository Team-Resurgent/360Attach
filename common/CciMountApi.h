// Interface between the Launcher and the Mount system DLL.
// mount.xex is loaded with XexLoadImage(XEX_LOADIMG_TYPE_SYSTEM_DLL) and its
// exports are resolved by ordinal with XexGetProcedureAddress.
#ifndef __CCIMOUNTAPI_H
#define __CCIMOUNTAPI_H

#define CCIMOUNT_MODULE_NAME		"mount.xex"
#define CCIMOUNT_VERSION			0x00010100	// 1.1: CciMountWithOverlay
#define CCIMOUNT_MAX_PARTS			8

#define CCIMOUNT_ORD_GETVERSION		1
#define CCIMOUNT_ORD_MOUNT			2
#define CCIMOUNT_ORD_UNMOUNT		3
#define CCIMOUNT_ORD_ISMOUNTED		4
#define CCIMOUNT_ORD_MOUNTOVERLAY	5

// A part is a .cci, or a raw .iso split by sector (name.1.iso, name.2.iso, ...).
// Paths are full kernel object paths, e.g.
// "\Device\Harddisk0\Partition1\Games\Terraria\Terraria.cci", one per part in
// part order. The files are opened from a system thread, so the handles stay
// valid after the calling title exits.
typedef DWORD	 (*PFN_CCIGETVERSION)( VOID );
typedef NTSTATUS (*PFN_CCIMOUNT)( const CHAR * const * partPaths, DWORD partCount );
typedef NTSTATUS (*PFN_CCIUNMOUNT)( VOID );
typedef BOOL	 (*PFN_CCIISMOUNTED)( VOID );

// Devkit testing: like CciMount, but reads of the disc's root default.xex are
// served from overlayPath instead, e.g. the same XEX re-signed for devkits
// (XexTool -m d). The overlay must be exactly the size of the file it replaces,
// otherwise it is ignored and the disc mounts unmodified.
typedef NTSTATUS (*PFN_CCIMOUNTOVERLAY)( const CHAR * const * partPaths, DWORD partCount, const CHAR * overlayPath );

#endif // __CCIMOUNTAPI_H
