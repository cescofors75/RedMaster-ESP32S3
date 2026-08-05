#include "daisy_pod.h"

#include <cstdint>

using namespace daisy;

namespace
{
DaisyPod hardware;
int32_t  encoder_page = 0;

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    // Passthrough oficial: ninguna ganancia, filtro, memoria ni efecto.
    for(size_t i = 0; i < size; ++i)
    {
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
    }
}

void ShowDiagnosticSignature()
{
    for(uint32_t i = 0; i < 3; ++i)
    {
        hardware.led1.Set(0.75f, 0.0f, 0.0f);
        hardware.led2.Set(0.0f, 0.75f, 0.0f);
        hardware.UpdateLeds();
        hardware.DelayMs(140);
        hardware.ClearLeds();
        hardware.UpdateLeds();
        hardware.DelayMs(90);
    }
}
} // namespace

int main(void)
{
    hardware.Init();
    hardware.SetAudioBlockSize(48);
    ShowDiagnosticSignature();
    hardware.StartAdc();
    hardware.StartAudio(AudioCallback);

    constexpr float colors[4][3]
        = {{0.0f, 0.55f, 1.0f},
           {1.0f, 0.04f, 0.35f},
           {1.0f, 0.50f, 0.0f},
           {0.50f, 0.0f, 1.0f}};

    while(true)
    {
        hardware.ProcessAllControls();
        encoder_page += hardware.encoder.Increment();
        while(encoder_page < 0)
            encoder_page += 4;
        encoder_page %= 4;

        const float knob1 = hardware.GetKnobValue(DaisyPod::KNOB_1);
        const float knob2 = hardware.GetKnobValue(DaisyPod::KNOB_2);
        const float level = 0.04f + 0.96f * knob1;
        const uint8_t page = static_cast<uint8_t>(encoder_page);

        hardware.led1.Set(colors[page][0] * level,
                          colors[page][1] * level,
                          colors[page][2] * level);
        hardware.led2.Set(hardware.button1.Pressed() ? 1.0f : 0.0f,
                          0.04f + knob2 * 0.96f,
                          hardware.button2.Pressed() ? 1.0f : 0.0f);
        hardware.UpdateLeds();
        hardware.DelayMs(1);
    }
}
