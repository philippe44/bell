#include "BellDSP.h"
#include <iostream>

using namespace bell;

BellDSP::BellDSP(){

};

void BellDSP::applyPipeline(std::shared_ptr<AudioPipeline> pipeline)
{
    std::scoped_lock lock(accessMutex);
    activePipeline = pipeline;
}

size_t BellDSP::process(uint8_t *data, size_t bytes, int channels,
                        SampleRate sampleRate, BitWidth bitWidth)
{
    // Create a StreamInfo object to pass to the pipeline
    auto streamInfo = std::make_unique<StreamInfo>();
    streamInfo->numChannels = channels;
    streamInfo->sampleRate = sampleRate;
    streamInfo->bitwidth = bitWidth;
    streamInfo->numSamples = bytes / 4;

    std::scoped_lock lock(accessMutex);
    if (activePipeline)
    {
        int16_t *data16Bit = (int16_t *)data;

        int length16 = bytes / 4;

        for (size_t i = 0; i < length16; i++)
        {
            dataLeft[i] = data16Bit[i * 2] / (float)MAX_INT16; // Normalize left
            dataRight[i] =
                data16Bit[i * 2 + 1] / (float)MAX_INT16; // Normalize right
        }
        float *sampleData[] = {&dataLeft[0], &dataRight[0]};
        streamInfo->data = sampleData;

        auto resultInfo = activePipeline->process(std::move(streamInfo));

        for (size_t i = 0; i < length16; i++)
        {
            if (dataLeft[i] > 1.0f)
            {
                std::cout << "Left Clipping!" << std::endl;
                dataLeft[i] = 1.0f;
            }

            // Data has been downmixed to mono
            if (resultInfo->numChannels == 1 && channels == 2)
            {
                data16Bit[i] = dataLeft[i] * MAX_INT16; // Denormalize left
            }
            else
            {
                if (dataRight[i] > 1.0f)
                {
                    std::cout << "Right Clipping!" << std::endl;
                    dataRight[i] = 1.0f;
                }
                data16Bit[i * 2] = dataLeft[i] * MAX_INT16;      // Denormalize left
                data16Bit[i * 2 + 1] = dataRight[i] * MAX_INT16; // Denormalize right
            }
        }

        if (resultInfo->numChannels == 1)
        {
            return bytes / 2;
        }
    }

    return bytes;
}

std::shared_ptr<AudioPipeline> BellDSP::getActivePipeline()
{
    return activePipeline;
}