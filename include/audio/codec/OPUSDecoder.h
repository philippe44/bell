// Copyright (c) Kuba Szczodrzyński 2022-1-14.

#pragma once

#include "BaseCodec.h"

struct OpusDecoder;

class OPUSDecoder : public BaseCodec {
  private:
	OpusDecoder *opus;
	int16_t *pcmData;

  public:
	OPUSDecoder();
	~OPUSDecoder();
	bool setup(uint32_t sampleRate, uint8_t channelCount, uint8_t bitDepth) override;
	uint8_t *decode(char *inData, size_t inLen, size_t &outLen) override;
};
