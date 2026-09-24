// C stand-ins for the x86-64 .asm helpers libvpx expects, for the assembler-free build
// described in GenerateLibvpxConfig.js.

#include "vpx_ports/system_state.h"

// vpx_ports/emms_mmx.asm issues EMMS to leave MMX state before x87 code runs. This build
// contains no MMX code at all - the hand-written assembly is excluded and MSVC offers no
// MMX intrinsics on x64 - so there is never any MMX state to clear.
void vpx_clear_system_state(void) {}
