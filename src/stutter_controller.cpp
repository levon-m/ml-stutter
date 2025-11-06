#include "stutter_controller.h"
#include "input_io.h"
#include "display_manager.h"
#include "timekeeper.h"
#include <Arduino.h>

StutterController::StutterController(AudioEffectStutter& effect)
    : m_effect(effect),
      m_currentParameter(Parameter::LENGTH) {
}

BitmapID StutterController::lengthToBitmap(StutterLength length) {
    switch (length) {
        case StutterLength::FREE:      return BitmapID::STUTTER_LENGTH_FREE;
        case StutterLength::QUANTIZED: return BitmapID::STUTTER_LENGTH_QUANT;
        default: return BitmapID::STUTTER_LENGTH_FREE;
    }
}

BitmapID StutterController::onsetToBitmap(StutterOnset onset) {
    switch (onset) {
        case StutterOnset::FREE:      return BitmapID::STUTTER_ONSET_FREE;
        case StutterOnset::QUANTIZED: return BitmapID::STUTTER_ONSET_QUANT;
        default: return BitmapID::STUTTER_ONSET_FREE;
    }
}

const char* StutterController::lengthName(StutterLength length) {
    switch (length) {
        case StutterLength::FREE:      return "Free";
        case StutterLength::QUANTIZED: return "Quantized";
        default: return "Free";
    }
}

const char* StutterController::onsetName(StutterOnset onset) {
    switch (onset) {
        case StutterOnset::FREE:      return "Free";
        case StutterOnset::QUANTIZED: return "Quantized";
        default: return "Free";
    }
}

bool StutterController::handleButtonPress(const Command& cmd) {
    if (cmd.targetEffect != EffectID::STUTTER) {
        return false;  // Not our effect
    }

    if (cmd.type != CommandType::EFFECT_ENABLE && cmd.type != CommandType::EFFECT_TOGGLE) {
        return false;  // Not a press command
    }

    StutterLength lengthMode = m_effect.getLengthMode();
    StutterOnset onsetMode = m_effect.getOnsetMode();

    if (onsetMode == StutterOnset::FREE) {
        // FREE ONSET: Engage immediately
        m_effect.enable();

        if (lengthMode == StutterLength::QUANTIZED) {
            // FREE ONSET + QUANTIZED LENGTH
            Quantization quant = EffectQuantization::getGlobalQuantization();
            uint32_t durationSamples = EffectQuantization::calculateQuantizedDuration(quant);
            uint64_t releaseSample = TimeKeeper::getSamplePosition() + durationSamples;
            m_effect.scheduleRelease(releaseSample);

            Serial.print("Stutter ENGAGED (Free onset, Quantized length=");
            Serial.print(EffectQuantization::quantizationName(quant));
            Serial.println(")");
        } else {
            // FREE ONSET + FREE LENGTH
            Serial.println("Stutter ENGAGED (Free onset, Free length)");
        }

        // Update visual feedback
        InputIO::setLED(EffectID::STUTTER, true);
        DisplayManager::instance().setLastActivatedEffect(EffectID::STUTTER);
        DisplayIO::showBitmap(BitmapID::STUTTER_ACTIVE);
        return true;  // Command handled
    } else {
        // QUANTIZED ONSET: Schedule for next boundary with lookahead offset
        Quantization quant = EffectQuantization::getGlobalQuantization();
        uint32_t samplesToNext = EffectQuantization::samplesToNextQuantizedBoundary(quant);

        // Apply lookahead offset (fire early to catch external audio transients)
        uint32_t lookahead = EffectQuantization::getLookaheadOffset();
        uint32_t adjustedSamples = (samplesToNext > lookahead) ? (samplesToNext - lookahead) : 0;

        // Calculate absolute sample position for onset
        uint64_t onsetSample = TimeKeeper::getSamplePosition() + adjustedSamples;

        // Schedule onset in ISR (same as how length scheduling works)
        m_effect.scheduleOnset(onsetSample);

        // If length is also quantized, schedule release from onset position
        if (lengthMode == StutterLength::QUANTIZED) {
            uint32_t durationSamples = EffectQuantization::calculateQuantizedDuration(quant);
            uint64_t releaseSample = onsetSample + durationSamples;
            m_effect.scheduleRelease(releaseSample);
        }

        Serial.print("Stutter ONSET scheduled (");
        Serial.print(EffectQuantization::quantizationName(quant));
        Serial.print(" grid, ");
        Serial.print(adjustedSamples);
        Serial.print(" samples, lookahead=");
        Serial.print(lookahead);
        Serial.println(")");

        return true;  // Command handled
    }
}

bool StutterController::handleButtonRelease(const Command& cmd) {
    if (cmd.targetEffect != EffectID::STUTTER) {
        return false;  // Not our effect
    }

    if (cmd.type != CommandType::EFFECT_DISABLE) {
        return false;  // Not a release command
    }

    StutterLength lengthMode = m_effect.getLengthMode();

    if (lengthMode == StutterLength::QUANTIZED) {
        // QUANTIZED LENGTH: Ignore release (auto-releases)
        Serial.println("Stutter button released (ignored - quantized length)");
        return true;  // Command handled (skip default disable)
    }

    // FREE LENGTH: Check if we have scheduled onset via ISR API
    // QUANTIZED ONSET + FREE LENGTH: Cancel scheduled onset
    m_effect.cancelScheduledOnset();
    Serial.println("Stutter scheduled onset CANCELLED (button released before beat)");

    // FREE ONSET + FREE LENGTH: Fall through to default disable
    return false;  // Let EffectManager handle disable
}

void StutterController::updateVisualFeedback() {
    // Check for ISR-fired onset (QUANTIZED ONSET mode)
    // Detect rising edge: Stutter enabled but display not showing it yet
    if (m_effect.isEnabled() && DisplayManager::instance().getLastActivatedEffect() != EffectID::STUTTER) {
        // ISR fired onset - update visual feedback
        InputIO::setLED(EffectID::STUTTER, true);
        DisplayManager::instance().setLastActivatedEffect(EffectID::STUTTER);
        DisplayIO::showBitmap(BitmapID::STUTTER_ACTIVE);

        // Determine what happened based on onset/length modes
        StutterOnset onsetMode = m_effect.getOnsetMode();
        StutterLength lengthMode = m_effect.getLengthMode();

        if (onsetMode == StutterOnset::QUANTIZED) {
            Quantization quant = EffectQuantization::getGlobalQuantization();
            Serial.print("Stutter ENGAGED at scheduled onset (");
            Serial.print(EffectQuantization::quantizationName(quant));
            Serial.print(" boundary, ");
            Serial.print(lengthMode == StutterLength::QUANTIZED ? "Quantized length)" : "Free length)");
            Serial.println();
        }
    }

    // Check for auto-release (QUANTIZED LENGTH mode)
    // Detect falling edge: Stutter disabled but display still showing it
    if (!m_effect.isEnabled() && DisplayManager::instance().getLastActivatedEffect() == EffectID::STUTTER) {
        // Only auto-release if in QUANTIZED length mode
        if (m_effect.getLengthMode() == StutterLength::QUANTIZED) {
            // Stutter auto-released - update display
            DisplayManager::instance().setLastActivatedEffect(EffectID::NONE);
            DisplayManager::instance().updateDisplay();

            // Update LED to reflect disabled state
            InputIO::setLED(EffectID::STUTTER, false);

            // Debug output
            Serial.println("Stutter auto-released (Quantized mode)");
        }
    }
}
