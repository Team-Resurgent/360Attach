// Virtual tray: broadcasts tray-open/tray-close SMC notifications so XAM and
// the kernel forget the previous disc and probe the newly mounted one.
#ifndef __VTRAY_H
#define __VTRAY_H

VOID VTrayStartup( VOID );
VOID VTrayCycle( VOID );

#endif // __VTRAY_H
