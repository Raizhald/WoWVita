// audio_out.h — the platform's stereo output for the NDSP mixer (ndsp.cpp).
#pragma once
#include <3ds/types.h>

namespace compat {

// render(out, frames, rate) must fill `frames` interleaved stereo s16 frames; called from the audio thread.
typedef void (*AudioRender)(s16* out, int frames, int rate);
bool audioStart(AudioRender render);
void audioStop();

}  // namespace compat
