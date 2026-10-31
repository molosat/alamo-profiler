// Vtable patches on the real D3D9 objects. Patching (rather than wrapping each
// interface) keeps object identity intact, so QueryInterface, D3DX and the
// game all see the runtime's own pointers.
#pragma once

#include <d3d9.h>

namespace hooks {

void PatchD3D9(IDirect3D9* d3d);
void PatchD3D9Ex(IDirect3D9Ex* d3d);

}  // namespace hooks
