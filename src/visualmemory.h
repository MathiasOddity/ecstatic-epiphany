/*
 * Experimental learning algorithm
 *
 * (c) 2014 Micah Elizabeth Scott
 * http://creativecommons.org/licenses/by/3.0/
 */

#pragma once

#include <vector>
#include "lib/camera_sampler.h"
#include "lib/effect_runner.h"
#include "lib/jpge.h"
#include "lib/lodepng.h"
#include "lib/prng.h"


class VisualMemory
{
public:
    void init(EffectRunner *runner);
    void process(const Camera::VideoChunk &chunk);

    void debug(const char *outputPngFilename);

private:
    typedef double memory_t;
    typedef std::vector<memory_t> memoryVector_t;

    PRNG random;
    EffectRunner *runner;
    std::vector<unsigned> denseToSparsePixelIndex;

    // Updated on the video thread constantly, via process()
    memoryVector_t memory;
    uint8_t samples[CameraSampler::kSamples];

    static const unsigned kLearnIterationsPerSample = 25;
    static const memory_t kLearningThreshold = 0.1f;
    static const memory_t kSmoothingRate = 1.0f;

    // Reinforce memories. Called from the video thread.
    void learn(memory_t &cell, const uint8_t *pixel, uint8_t luminance);

    // Smoothing over one axis. Pushes towards a version of the signal with DC
    // components removed along axis A. Remove DC signals from one axis (A) while iterating
    // over another axis (B).
    void smooth(unsigned stepA, unsigned countA, unsigned stepB, unsigned countB, memory_t gain);
};


/*****************************************************************************************
 *                                   Implementation
 *****************************************************************************************/


inline void VisualMemory::init(EffectRunner *runner)
{
    this->runner = runner;
    const Effect::PixelInfoVec &pixelInfo = runner->getPixelInfo();

    // Make a densely packed pixel index, skipping any unmapped pixels

    denseToSparsePixelIndex.clear();
    for (unsigned i = 0; i < pixelInfo.size(); ++i) {
        const Effect::PixelInfo &pixel = pixelInfo[i];
        if (pixel.isMapped()) {
            denseToSparsePixelIndex.push_back(i);
        }
    }

    // Calculate size of full visual memory

    unsigned denseSize = denseToSparsePixelIndex.size();
    unsigned cells = CameraSampler::kSamples * denseSize;
    fprintf(stderr, "vismem: %d camera samples * %d LED pixels = %d cells\n",
        CameraSampler::kSamples, denseSize, cells);

    memory.resize(cells);
    random.seed(27);
}

inline void VisualMemory::process(const Camera::VideoChunk &chunk)
{
    EffectRunner *runner = this->runner;
    CameraSampler sampler(chunk);
    unsigned sampleIndex;
    uint8_t luminance;
    unsigned denseSize = denseToSparsePixelIndex.size();

    // First, iterate over camera samples
    while (sampler.next(sampleIndex, luminance)) {
        memory_t *row = &memory[sampleIndex * denseSize];

        // Save the sample's luma value for later
        samples[sampleIndex] = luminance;

        // Randomly select kLearnIterationsPerSample LEDs to learn with
        for (unsigned iter = kLearnIterationsPerSample; iter; --iter) {
            unsigned denseIndex = (uint64_t(random.uniform32()) * denseSize) >> 32;
            unsigned sparseIndex = denseToSparsePixelIndex[denseIndex];

            // Single memory cell and pixel
            const uint8_t* pixel = runner->getPixel(sparseIndex);
            memory_t &cell = row[denseIndex];

            // Iterative learning algorithm
            learn(cell, pixel, luminance);
        }
    }
}

inline void VisualMemory::learn(memory_t &cell, const uint8_t *pixel, uint8_t luminance)
{
    uint8_t r = pixel[0];
    uint8_t g = pixel[1];
    uint8_t b = pixel[2];

    memory_t lumaSq = (int(luminance) * int(luminance)) / 65025.0;
    memory_t pixSq = int(r*r + g*g + b*b) / 195075.0;
    memory_t reinforcement = lumaSq * pixSq;

    if (reinforcement > kLearningThreshold) {
        cell += lumaSq * pixSq;
    }
}

inline void VisualMemory::smooth(unsigned stepA, unsigned countA,
    unsigned stepB, unsigned countB, memory_t gain)
{
    // Normalize, remove DC signals from one axis (A) while iterating
    // over another axis (B).

    unsigned indexB = 0, remainingB = countB;
    while (remainingB) {

        // First pass, sum over axis A

        memory_t total = 0;
        unsigned indexA = indexB, remainingA = countA;
        while (remainingA) {
            total += memory[indexA];
            indexA += stepA;
            remainingA--;
        }

        // Now subtract the average

        memory_t adjustment = gain * -total / countA;
        indexA = indexB;
        remainingA = countA;
        while (remainingA) {
            memory[indexA] += adjustment;
            indexA += stepA;
            remainingA--;
        }

        indexB += stepB;
        remainingB--;
    }
}

inline void VisualMemory::debug(const char *outputPngFilename)
{
    unsigned denseSize = denseToSparsePixelIndex.size();

    // XXX smoothing
    smooth(1, denseSize, denseSize, CameraSampler::kSamples, kSmoothingRate);
    smooth(denseSize, CameraSampler::kSamples, 1, denseSize, kSmoothingRate);

    // Tiled array of camera samples, one per LED. Artificial square grid of LEDs.
    const int ledsWide = 16;
    const int width = ledsWide * CameraSampler::kBlocksWide;
    const int ledsHigh = (denseToSparsePixelIndex.size() + ledsWide - 1) / ledsWide;
    const int height = ledsHigh * CameraSampler::kBlocksHigh;
    std::vector<uint8_t> image;
    image.resize(width * height);

    // Extents
    memory_t cellMin = memory[0];
    memory_t cellMax = memory[0];
    for (unsigned cell = 1; cell < memory.size(); cell++) {
        memory_t v = memory[cell];
        cellMin = std::min(cellMin, v);
        cellMax = std::max(cellMax, v);
    }
    memory_t cellScale = 255 / (cellMax - cellMin);
    fprintf(stderr, "vismem: range [%f, %f]\n", cellMin, cellMax);

    for (unsigned sample = 0; sample < CameraSampler::kSamples; sample++) {
        for (unsigned led = 0; led < denseSize; led++) {

            int sx = CameraSampler::blockX(sample);
            int sy = CameraSampler::blockY(sample);

            int x = sx + (led % ledsWide) * CameraSampler::kBlocksWide;
            int y = sy + (led / ledsWide) * CameraSampler::kBlocksHigh;

            const memory_t &cell = memory[ sample * denseSize + led ];
            uint8_t *pixel = &image[ y * width + x ];

            *pixel = (cell - cellMin) * cellScale;
        }
    }

    if (lodepng_encode_file(outputPngFilename, &image[0], width, height, LCT_GREY, 8)) {
        fprintf(stderr, "vismem: error saving %s\n", outputPngFilename);
    } else {
        fprintf(stderr, "vismem: saved %s\n", outputPngFilename);
    }
}
