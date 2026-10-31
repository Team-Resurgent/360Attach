// Redirects a kernel export, named by ordinal, to a replacement routine by
// rewriting the entry in the kernel module's export address table. New callers
// that resolve the export (a freshly launched title binding its imports) reach
// the replacement; the kernel image itself is otherwise untouched.
#ifndef __KERNELHOOK_H
#define __KERNELHOOK_H

// Returns the export's original absolute address, to chain to and to pass back
// to KernelUnhookExport, or 0 if the table could not be patched.
DWORD KernelHookExport( DWORD ordinal, PVOID replacement );

// Restores an entry patched by KernelHookExport.
VOID KernelUnhookExport( DWORD ordinal, DWORD original );

#endif // __KERNELHOOK_H
