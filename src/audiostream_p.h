/*
  SDL_audiolib - An audio decoding, resampling and mixing library
  Copyright (C) 2014  Nikos Chantziaras

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/
#pragma once

#include <memory>
#include <vector>
#include <queue>
#include <SDL_audio.h>
#include <aulib.h>
#include "Buffer.h"

namespace Aulib {

/// \private
struct AudioStream_priv final {
    const class AudioStream* const q;

    explicit AudioStream_priv(class AudioStream* pub, std::unique_ptr<class AudioDecoder> decoder,
                              std::unique_ptr<class AudioResampler> resampler, bool closeRw);
    ~AudioStream_priv();

    bool fIsOpen = false;
    SDL_RWops* fRWops;
    bool fCloseRw;
    // Resamplers hold a reference to decoders, so we store it as a shared_ptr.
    std::shared_ptr<AudioDecoder> fDecoder;
    std::unique_ptr<AudioResampler> fResampler;
    bool fIsPlaying = false;
    bool fIsPaused = false;
    float fVolume = 1.f;
    float fInternalVolume = 1.f;
    int fCurrentIteration = 0;
    int fWantedIterations = 0;
    int fPlaybackStartTick = 0;
    int fFadeInStartTick = 0;
    int fFadeOutStartTick = 0;
    bool fFadingIn = false;
    bool fFadingOut = false;
    bool fStopAfterFade = false;
    int fFadeInTickDuration = 0;
    int fFadeOutTickDuration = 0;

    static ::SDL_AudioSpec fAudioSpec;
    static std::vector<AudioStream*> fStreamList;

    // This points to an appropriate converter for the current audio format.
    static void (*fSampleConverter)(Uint8[], const Buffer<float>& src);

    // Sample buffers we use during decoding and mixing.
    static Buffer<float> fFinalMixBuf;
    static Buffer<float> fStrmBuf;

    void fProcessFade();
    void fStop();

    static void fSdlCallbackImpl(void*, Uint8 out[], int outLen);
};

} // namespace Aulib
