#pragma once

#include "audio_effect_base.h"
#include "timekeeper.h"
#include <atomic>
#include <Arduino.h>

enum class StutterLength : uint8_t {
    FREE = 0,       // Release immediately when button released (default)
    QUANTIZED = 1   // Auto-release after global quantization duration
};

enum class StutterOnset : uint8_t {
    FREE = 0,       // Engage immediately when button pressed (default)
    QUANTIZED = 1   // Quantize onset to next beat/subdivision
};

enum class StutterCaptureStart : uint8_t {
    FREE = 0,       // Engage immediately when button pressed (default)
    QUANTIZED = 1   // Quantize onset to next beat/subdivision
};

enum class StutterCaptureEnd : uint8_t {
    FREE = 0,       // Engage immediately when button pressed (default)
    QUANTIZED = 1   // Quantize onset to next beat/subdivision
};

enum class StutterState : uint8_t {
    IDLE = 0,
    CAPTURING = 1,
    PLAYING = 2,
    ARMED = 3 //armed for onset
};

class AudioEffectStutter : public AudioEffectBase {
public:
    AudioEffectStutter() : AudioEffectBase(2) {  // Call base with 2 inputs (stereo)
        m_writePos = 0;
        m_readPos = 0;
        //m_isEnabled.store(false, std::memory_order_relaxed);  // Start disabled (passthrough)
        m_state = StutterState::IDLE;
        m_lengthMode = StutterLength::FREE;  // Default: free mode
        m_onsetMode = StutterOnset::FREE;    // Default: free mode
        m_captureStartMode = StutterCaptureStart::FREE;    // Default: free mode
        m_captureEndMode = StutterCaptureEnd::FREE;    // Default: free mode
        m_releaseAtSample = 0;  // No scheduled release
        m_onsetAtSample = 0;    // No scheduled onset
        m_captureStartAtSample = 0;
        m_captureEndAtSample = 0;

        // Initialize buffers to silence
        memset(m_stutterBufferL, 0, sizeof(m_stutterBufferL));
        memset(m_stutterBufferR, 0, sizeof(m_stutterBufferR));
    }

    void enable() override {
        // Set read position to current write position
        // This captures the most recent audio in the buffer
        m_readPos = m_writePos;
        //m_isEnabled.store(true, std::memory_order_release);
        m_state = StutterState::PLAYING;
    }

    void disable() override {
        //m_isEnabled.store(false, std::memory_order_release);
        m_state = StutterState::IDLE;
    }

    void toggle() override {
        if (isEnabled()) {
            disable();
        } else {
            enable();
        }
    }

    bool isEnabled() const override {
        //return m_isEnabled.load(std::memory_order_acquire);
        return m_state;
    }

    const char* getName() const override {
        return "Stutter";
    }

    void setLengthMode(StutterLength mode) {
        m_lengthMode = mode;
    }

    StutterLength getLengthMode() const {
        return m_lengthMode;
    }

    void scheduleRelease(uint64_t releaseSample) {
        m_releaseAtSample = releaseSample;
    }

    // void cancelScheduledRelease() {
    //     m_releaseAtSample = 0;
    // }

    void scheduleOnset(uint64_t onsetSample) {
        m_onsetAtSample = onsetSample;
    }

    void cancelScheduledOnset() {
        m_onsetAtSample = 0;
    }

    void setOnsetMode(StutterOnset mode) {
        m_onsetMode = mode;
    }

    StutterOnset getOnsetMode() const {
        return m_onsetMode;
    }

    virtual void update() override {
        uint64_t currentSample = TimeKeeper::getSamplePosition();
        uint64_t blockEndSample = currentSample + AUDIO_BLOCK_SAMPLES;

        // Check for scheduled onset (ISR-accurate quantized onset)
        // Fire if the scheduled sample falls within this audio block [currentSample, blockEndSample)
        if (m_onsetAtSample > 0 && m_onsetAtSample >= currentSample && m_onsetAtSample < blockEndSample) {
            // Time to engage stutter (block-accurate - best we can do in ISR)
            m_readPos = m_writePos;  // Capture current buffer position

            //m_isEnabled.store(true, std::memory_order_release);
            m_state = StutterState::PLAYING;

            m_onsetAtSample = 0;  // Clear scheduled onset
        }

        // Check for scheduled release (ISR-accurate quantized length)
        // Fire if the scheduled sample falls within this audio block [currentSample, blockEndSample)
        if (m_releaseAtSample > 0 && m_releaseAtSample >= currentSample && m_releaseAtSample < blockEndSample) {

            // Time to auto-release (block-accurate)
            //m_isEnabled.store(false, std::memory_order_release);
            m_state = StutterState::IDLE;


            m_releaseAtSample = 0;  // Clear scheduled release
        }

        switch (m_state) {
            // STUTTER IS IDLE
            case StutterState::IDLE:
                // PASSTHROUGH MODE: Record to buffer and pass through
                audio_block_t* blockL = receiveWritable(0);
                audio_block_t* blockR = receiveWritable(1);

                if (blockL && blockR) {
                    // Write to circular buffer (continuously recording)
                    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                        m_stutterBufferL[m_writePos] = blockL->data[i];
                        m_stutterBufferR[m_writePos] = blockR->data[i];

                        // Advance write position (circular)
                        m_writePos++;
                        if (m_writePos >= STUTTER_BUFFER_SAMPLES) {
                            m_writePos = 0;
                        }
                    }

                    // Pass through unmodified
                    transmit(blockL, 0);
                    transmit(blockR, 1);
                }

                // Release blocks (if allocated)
                if (blockL) release(blockL);
                if (blockR) release(blockR);
                break;
            // STUTTER IS CAPTURING
            case StutterState::CAPTURING:
                //write to non-circular buffer, when it is filled, stop capturing. Other logic should boot us out of this state once buffer is filled (in manager/controller?)
                //or, do we goto PLAYING when buffer gets filled here?
                audio_block_t* blockL = receiveWritable(0);
                audio_block_t* blockR = receiveWritable(1);

                if ((blockL && blockR) && m_writePos < STUTTER_BUFFER_SAMPLES) {
                    // Write to buffer
                    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                        m_stutterBufferL[m_writePos] = blockL->data[i];
                        m_stutterBufferR[m_writePos] = blockR->data[i];

                        // Advance write position
                        m_writePos++;
                        //if (m_writePos >= STUTTER_BUFFER_SAMPLES) {
                        //    m_writePos = 0;
                        //}
                    }

                    // Pass through unmodified
                    transmit(blockL, 0);
                    transmit(blockR, 1);
                }

                // Release blocks (if allocated)
                if (blockL) release(blockL);
                if (blockR) release(blockR);
                break;
            // STUTTER IS PLAYING
            // repeat buffer, onset/length stuff is not handled here.
            case StutterState::PLAYING:
                // FROZEN MODE: Read from buffer and loop
                audio_block_t* outL = allocate();
                audio_block_t* outR = allocate();

                if (outL && outR) {
                    // Read from buffer
                    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                        outL->data[i] = m_stutterBufferL[m_readPos];
                        outR->data[i] = m_stutterBufferR[m_readPos];

                        // Advance read position
                        m_readPos++;
                        //if (m_readPos >= STUTTER_BUFFER_SAMPLES) {
                        //    m_readPos = 0;  // Loop back to start
                        //}
                    }

                    // Transmit frozen audio
                    transmit(outL, 0);
                    transmit(outR, 1);
                }

                // Release output blocks
                if (outL) release(outL);
                if (outR) release(outR);

                // Consume and discard input blocks (we're not using them)
                audio_block_t* blockL = receiveReadOnly(0);
                audio_block_t* blockR = receiveReadOnly(1);
                if (blockL) release(blockL);
                if (blockR) release(blockR);
                break;    
        }
    }

private:
    // buffer size would not be in compile time, changes when BPM changes. TimeKeeper::getSamplesPerBeat
    // probably set a large max buffer (1 beat @ a BPM I likely wont exceed like 200) (Wait, shoudln't we base it on min bpm? lower bpm = more samples per beat)
    // also do 1 bar (* 4)
    static constexpr uint8_t MIN_TEMPO = 70;
    static constexpr size_t STUTTER_BUFFER_SAMPLES = static_cats<size_t>((1 / (MIN_TEMPO / 60.0)) * SAMPLE_RATE) * 4;

    //std::array instead, same layout, size is known, algorithms-friendly, stronger typing
    //if DMA/cahce alignment matters, use alignas()
    //make it static, global, or in a dedicated secrtion

    //Maybe put in RAM2(OCRAM). 32-bit alignment minimizes cache line thrash and helps when any DMA is involved
    //DMAMEM alignas(32) std::array<int16_t, STUTTER_BUFFER_SAMPLES> m_stutterBufferL;

    //~295KB
    //std::array<int16_t, STUTTER_BUFFER_SAMPLES> m_stutterBufferL;
    //std::array<int16_t, STUTTER_BUFFER_SAMPLES> m_stutterBufferR;
    int16_t m_stutterBufferL[STUTTER_BUFFER_SAMPLES];
    int16_t m_stutterBufferR[STUTTER_BUFFER_SAMPLES];

    size_t m_writePos;
    size_t m_readPos;

    //std::atomic<bool> m_isEnabled;
    StutterState m_state;

    // stutter length mode state
    StutterLength m_lengthMode;        // FREE or QUANTIZED
    uint64_t m_releaseAtSample;       // Sample position when stutter should auto-release (0 = no scheduled release)

    // stutter onset mode state
    StutterOnset m_onsetMode;          // FREE or QUANTIZED
    uint64_t m_onsetAtSample;         // Sample position when stutter should engage (0 = no scheduled onset)

    // stutter capture start mode state
    StutterCaptureStart m_captureStartMode;
    uint64_t m_captureStartAtSample = 0;

    // stutter capture end mode state
    StutterCaptureEnd m_captureEndMode;
    uint64_t m_captureEndAtSample = 0;
};
