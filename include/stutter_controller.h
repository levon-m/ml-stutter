#pragma once

#include "effect_controller.h"
#include "audio_stutter.h"
#include "effect_quantization.h"
#include "display_io.h"

class StutterController : public IEffectController {
public:
    /**
     * Parameter selection for encoder editing
     */
    enum class Parameter : uint8_t {
        LENGTH = 0,
        ONSET = 1,
        C_START = 2,
        C_END = 3
    };

    /**
     * Constructor
     *
     * @param effect Reference to the freeze audio effect
     */
    explicit StutterController(AudioEffectStutter& effect);

    // IEffectController interface implementation
    bool handleButtonPress(const Command& cmd) override;
    bool handleButtonRelease(const Command& cmd) override;
    void updateVisualFeedback() override;
    EffectID getEffectID() const override { return EffectID::STUTTER; }

    /**
     * Get current parameter being edited
     */
    Parameter getCurrentParameter() const { return m_currentParameter; }

    /**
     * Set current parameter to edit
     */
    void setCurrentParameter(Parameter param) { m_currentParameter = param; }

    // Utility functions for bitmap/name mapping
    static BitmapID lengthToBitmap(StutterLength length);
    static BitmapID onsetToBitmap(StutterOnset onset);
    static const char* lengthName(StutterLength length);
    static const char* onsetName(StutterOnset onset);

private:
    AudioEffectStutter& m_effect;    // Reference to audio effect (DSP)
    Parameter m_currentParameter;   // Currently selected parameter for editing
};
