// CdRom0 hook. Swaps the DRIVER_OBJECT behind \Device\Cdrom0 for one whose
// read and device-control routines serve the mounted CCI, and that passes
// everything to the real drive driver while nothing is mounted.
#ifndef __DVDHOOK_H
#define __DVDHOOK_H

NTSTATUS DvdHookStartup( VOID );
VOID	 DvdHookShutdown( VOID );

#endif // __DVDHOOK_H
