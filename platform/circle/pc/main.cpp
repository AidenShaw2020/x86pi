#include "kernel.h"
#include <circle/startup.h>
int main()
{
    CKernel kernel;
    if (kernel.Initialize()) kernel.Run();
    halt();
    return EXIT_HALT;
}
