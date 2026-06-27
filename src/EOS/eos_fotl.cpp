// eos_ftol.cpp -- __ftol2_sse shim.
// RXDK / MSVC2003 with /GL and no CRT references __ftol2_sse for float->int
// conversions. Provide the standard x87 fistp shim. If you already link your
// own (e.g. dd_ftol.cpp from DarkDash), drop this file from the project.
extern "C" __declspec(naked) void __ftol2_sse()
{
    __asm
    {
        fistp dword ptr[esp - 4]
        mov   eax, dword ptr[esp - 4]
        ret
    }
}