// Lets a disc title that demands AP25 media authentication run from a mounted
// image. AP25 verifies challenge data only a genuine pressed disc carries, so a
// title that requires it hangs to a black screen once a profile is signed in
// and the check is enforced. The hook reports that one requirement as absent.
#ifndef __AUTHHOOK_H
#define __AUTHHOOK_H

VOID AuthHookInstall( VOID );
VOID AuthHookRemove( VOID );

#endif // __AUTHHOOK_H
