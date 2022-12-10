#pragma once

#include <atomic>
#include <cmath>
#include <memory>

#include "CircularBuffer.h"
#include "StreamInfo.h"

typedef std::function<void(std::string)> shutdownEventHandler;

namespace bell {
class CentralAudioBuffer {
  private:
	std::shared_ptr<bell::CircularBuffer> audioBuffer;
	std::mutex accessMutex;

	uint32_t sampleRate		   = 0;
	std::atomic<bool> isLocked = false;

  public:
	CentralAudioBuffer(size_t size) {
		audioBuffer = std::make_shared<CircularBuffer>(size);
	}

	/**
	 * Sends an event which reconfigures current audio output
	 * @param format incoming sample format
	 * @param sampleRate data's sample rate
	 */
	void configureOutput(bell::BitWidth format, uint32_t sampleRate) {
		if (this->sampleRate != sampleRate) {
			this->sampleRate = sampleRate;
		}
	}

	/**
	 * Returns current sample rate
	 * @return sample rate
	 */
	uint32_t getSampleRate() {
		return sampleRate;
	}

	/**
	 * Clears input buffer, to be called for track change and such
	 */
	void clearBuffer() {
		audioBuffer->emptyExcept(this->sampleRate);
	}

	/**
	 * Locks access to audio buffer. Call after starting playback
	 */
	void lockAccess() {
		if (!isLocked) {
			clearBuffer();
			this->accessMutex.lock();
			isLocked = true;
		}
	}

	/**
	 * Frees access to the audio buffer. Call during shutdown
	 */
	void unlockAccess() {
		if (isLocked) {
			clearBuffer();
			this->accessMutex.unlock();
			isLocked = false;
		}
	}

	/**
	 * Write audio data to the main buffer
	 * @param data pointer to raw PCM data
	 * @param bytes number of bytes to be read from provided pointer
	 * @return number of bytes read
	 */
	size_t write(const uint8_t *data, size_t bytes) {
		size_t bytesWritten = 0;
		while (bytesWritten < bytes) {
			auto write = audioBuffer->write(data + bytesWritten, bytes - bytesWritten);
			bytesWritten += write;

			if (write == 0) {
                audioBuffer->dataSemaphore->wait();
			}
		}

		return bytesWritten;
	}
};

} // namespace bell