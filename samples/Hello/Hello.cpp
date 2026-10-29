#include <xtl.h>

extern "C" void DbgPrint(const char*, ...);

// Prints a counter once a second, so the kit's debug output shows the title is running.
int __cdecl main()
{
    for (unsigned int i = 0; ; i++)
    {
        DbgPrint("[Hello] tick %u\n", i);
        Sleep(1000);
    }
    return 0;
}
