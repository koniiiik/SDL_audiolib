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
#include "Aulib/AudioResampler.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <SDL_audio.h>
#include "Aulib/AudioDecoder.h"
#include "aulib_global.h"
#include "aulib_debug.h"
#include "SdlAudioLocker.h"
#include "Buffer.h"


/* Relocate any samples in the specified buffer to the beginning:
 *
 *      ....ssss  ->  ssss....
 *
 * The tracking indices are adjusted as needed.
 */
static void
relocateBuffer(float* buf, size_t& pos, size_t& end)
{
    if (end == 0) {
        return;
    }
    if (pos >= end) {
        pos = end = 0;
        return;
    }
    if (pos == 0) {
        return;
    }
    size_t len = end - pos;
    memmove(buf, buf + pos, len * sizeof(*buf));
    pos = 0;
    end = len;
}


namespace Aulib {

/// \private
class AudioResampler_priv {
    friend class AudioResampler;
    AudioResampler* q;

    AudioResampler_priv(AudioResampler *pub);
    ~AudioResampler_priv();

    AudioDecoder* fDecoder;
    unsigned fDstRate;
    unsigned fSrcRate;
    unsigned fChannels;
    unsigned fChunkSize;
    Buffer<float> fOutBuffer;
    Buffer<float> fInBuffer;
    size_t fOutBufferPos;
    size_t fOutBufferEnd;
    size_t fInBufferPos;
    size_t fInBufferEnd;
    bool fPendingSpecChange;

    /* Move at most 'dstLen' samples from the output buffer into 'dst'.
     *
     * Returns the amount of samples that were actually moved.
     */
    size_t fMoveFromOutBuffer(float dst[], size_t dstLen);

    /* Adjust all internal buffer sizes for the current source and target
     * sampling rates.
     */
    void fAdjustBufferSizes();

    /* Resample samples from the input buffer and move them to the output
     * buffer.
     */
    void fResampleFromInBuffer();
};

} // namespace Aulib


Aulib::AudioResampler_priv::AudioResampler_priv(AudioResampler* pub)
    : q(pub),
      fDecoder(nullptr),
      fDstRate(0),
      fSrcRate(0),
      fChannels(0),
      fChunkSize(0),
      fOutBuffer(0),
      fInBuffer(0),
      fOutBufferPos(0),
      fOutBufferEnd(0),
      fInBufferPos(0),
      fInBufferEnd(0),
      fPendingSpecChange(false)
{
}


Aulib::AudioResampler_priv::~AudioResampler_priv()
{ }


size_t
Aulib::AudioResampler_priv::fMoveFromOutBuffer(float dst[], size_t dstLen)
{
    if (fOutBufferEnd == 0) {
        return 0;
    }
    if (fOutBufferPos >= fOutBufferEnd) {
        fOutBufferPos = fOutBufferEnd = 0;
        return 0;
    }
    size_t len = std::min(fOutBufferEnd - fOutBufferPos, dstLen);
    memcpy(dst, fOutBuffer.data() + fOutBufferPos, len * sizeof(*fOutBuffer.data()));
    fOutBufferPos += len;
    if (fOutBufferPos >= fOutBufferEnd) {
        fOutBufferEnd = fOutBufferPos = 0;
    }
    return len;
}


void
Aulib::AudioResampler_priv::fAdjustBufferSizes()
{
    size_t oldInBufLen = fInBufferEnd - fInBufferPos;
    size_t outBufSiz = fChannels * fChunkSize;
    size_t inBufSiz;

    if (fDstRate == fSrcRate) {
        // In the no-op case where we don't actually resample, input and output
        // buffers have the same size, since we're just copying the samples
        // as-is from input to output.
        inBufSiz = outBufSiz;
    } else {
        // When resampling, the input buffer's size depends on the ratio between
        // the source and destination sample rates.
        inBufSiz = std::ceil((double)outBufSiz * fSrcRate / fDstRate);
        size_t remainder = inBufSiz % fChannels;
        if (remainder) {
            inBufSiz = inBufSiz + fChannels - remainder;
        }
    }

    fOutBuffer.reset(outBufSiz);
    fInBuffer.resize(inBufSiz);
    fOutBufferPos = fOutBufferEnd = fInBufferPos = 0;
    if (oldInBufLen != 0) {
        fInBufferEnd = oldInBufLen;
    } else {
        fInBufferEnd = 0;
    }
}


void
Aulib::AudioResampler_priv::fResampleFromInBuffer()
{
    int inLen = fInBufferEnd - fInBufferPos;
    float* from = fInBuffer.data() + fInBufferPos;
    float* to = fOutBuffer.data() + fOutBufferEnd;
    if (fSrcRate == fDstRate) {
        // No resampling is needed. Just copy the samples as-is.
        int outLen = std::min(fOutBuffer.size() - fOutBufferEnd, (size_t)inLen);
        std::memcpy(to, from, outLen * sizeof(*from));
        fOutBufferEnd += outLen;
        fInBufferPos += outLen;
    } else {
        int outLen = fOutBuffer.size() - fOutBufferEnd;
        q->doResampling(to, from, outLen, inLen);
        fOutBufferEnd += outLen;
        fInBufferPos += inLen;
    }
    if (fInBufferPos >= fInBufferEnd) {
        // No more samples left to resample. Mark the input buffer as empty.
        fInBufferPos = fInBufferEnd = 0;
    }
}


Aulib::AudioResampler::AudioResampler()
    : d(new AudioResampler_priv(this))
{
}


Aulib::AudioResampler::~AudioResampler()
{
    delete d;
}


void
Aulib::AudioResampler::setDecoder(AudioDecoder* decoder)
{
    SdlAudioLocker locker;
    d->fDecoder = decoder;
}


int
Aulib::AudioResampler::setSpec(unsigned dstRate, unsigned channels, unsigned chunkSize)
{
    d->fDstRate = dstRate;
    d->fChannels = channels;
    d->fChunkSize = chunkSize;
    d->fSrcRate = d->fDecoder->getRate();
    d->fSrcRate = std::min(std::max(4000U, d->fSrcRate), 192000U);
    d->fAdjustBufferSizes();
    // Inform our child class about the spec change.
    adjustForOutputSpec(d->fDstRate, d->fSrcRate, d->fChannels);
    return 0;
}


unsigned
Aulib::AudioResampler::currentRate() const
{
    return d->fDstRate;
}


unsigned
Aulib::AudioResampler::currentChannels() const
{
    return d->fChannels;
}


unsigned
Aulib::AudioResampler::currentChunkSize() const
{
    return d->fChunkSize;
}


int
Aulib::AudioResampler::resample(float dst[], int dstLen)
{
    size_t totalSamples = 0;
    bool decEOF = false;

    if (d->fPendingSpecChange) {
        // There's a spec change pending. Process any data that is still in our
        // buffers using the current spec.
        totalSamples += d->fMoveFromOutBuffer(dst, dstLen);
        relocateBuffer(d->fOutBuffer.data(), d->fOutBufferPos, d->fOutBufferEnd);
        d->fResampleFromInBuffer();
        if (totalSamples >= dstLen) {
            // There's still samples left in the output buffer, so don't change
            // the spec yet.
            return dstLen;
        }
        // Our buffers are empty, so we can change to the new spec.
        setSpec(d->fDstRate, d->fChannels, d->fChunkSize);
        d->fPendingSpecChange = false;
    }

    // Keep resampling until we either produce the requested amount of output
    // samples, or the decoder has no more samples to give us.
    while (totalSamples < dstLen and not decEOF) {
        // If the input buffer is not filled, get some more samples from the
        // decoder.
        if (d->fInBufferEnd < d->fInBuffer.size()) {
            bool callAgain = false;
            int decSamples = d->fDecoder->decode(d->fInBuffer.data() + d->fInBufferEnd,
                                                 d->fInBuffer.size() - d->fInBufferEnd,
                                                 callAgain);
            // If the decoder indicated a spec change, process any data that is
            // still in our buffers using the current spec.
            if (callAgain) {
                d->fInBufferEnd += decSamples;
                d->fResampleFromInBuffer();
                totalSamples += d->fMoveFromOutBuffer(dst + totalSamples, dstLen - totalSamples);
                if (totalSamples >= dstLen) {
                    // There's still samples left in the output buffer. Keep
                    // the current spec and prepare to change it on our next
                    // call.
                    d->fPendingSpecChange = true;
                    return dstLen;
                }
                setSpec(d->fDstRate, d->fChannels, d->fChunkSize);
            } else if (decSamples <= 0) {
                decEOF = true;
            } else {
                d->fInBufferEnd += decSamples;
            }
        }

        d->fResampleFromInBuffer();
        relocateBuffer(d->fInBuffer.data(), d->fInBufferPos, d->fInBufferEnd);
        totalSamples += d->fMoveFromOutBuffer(dst + totalSamples, dstLen - totalSamples);
        relocateBuffer(d->fOutBuffer.data(), d->fOutBufferPos, d->fOutBufferEnd);
    }
    return totalSamples;
}
