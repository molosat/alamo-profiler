// GPU frame timing with D3D9 timestamp queries.
//
// gpu_ms is the GPU-clock span from the frame's first BeginScene/Clear to just
// before Present. It includes any time the GPU sat idle waiting on the CPU, so
// read it as "GPU span", not "GPU busy": if the game is CPU-bound it shrinks
// toward the real GPU work; if it is GPU-bound it approaches frame_ms.
#pragma once

#include <d3d9.h>

#include <cstdint>

namespace gpu {

// Creates the query ring on this device (replacing any previous one).
// Leaves timing disabled if the driver lacks timestamp queries.
void Attach(IDirect3DDevice9* dev);
// Releases every query. Safe to call twice.
void Detach();

IDirect3DDevice9* Device();
// Device references our queries hold (measured, not assumed).
unsigned long HeldDeviceRefs();

void BeginFrame();   // first BeginScene/Clear of a frame
void EndFrame();     // right before Present
void TagLastFrame(uint64_t frame);
void Poll();         // non-blocking readback into profiler::Stats()

}  // namespace gpu
