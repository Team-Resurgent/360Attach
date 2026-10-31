// Hooks the kernel's XeKeysGetMediaID so that, while a disc is mounted, callers
// get the mounted image's media ID instead of the (empty) physical drive's. A
// signed-in profile's content, update and licence checks query the media ID
// when a disc title starts; without this they read a blank or failed value and
// the title can fail to launch. The disc image itself is served unchanged, so a
// title update still matches.
#ifndef __MEDIAIDHOOK_H
#define __MEDIAIDHOOK_H

VOID MediaIdHookInstall( VOID );
VOID MediaIdHookRemove( VOID );

#endif // __MEDIAIDHOOK_H
