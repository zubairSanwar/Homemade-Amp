/*
Non-Linear Effect Header.

Includes:
- Gain
- Distortion
- Noise Gate
*/
#pragma once

#include <atomic>

struct Gain{
    std::atomic<float> new_gain{1.0f};  //gain factor
    std::atomic<float> smooth{0.002f};  //smoothing coefficient for a click-free knob
    float current_gain = 1.0f;

    float applyGain(float sample);
};

struct Distortion{
    std::atomic<float> new_drive{1.0f};     //target drive
    std::atomic<float> smooth{0.002f};
    std::atomic<float> out_level{1.0f};
    float cur_drive = 1.0f;

    float applyDistortion(float sample);
};

struct NoiseGate{
    //kill signal when not playing to stop buzz
    std::atomic<float> threshold{0.00316f};  //-50 dBFS

    float env = 0.0f;
    float g   = 0.0f;       //current multiplier: 0 = shut, 1 = fully open
    float attack = 0.0f, release = 0.0f, envFall = 0.0f;

    void init(float sampleRate);
    float applyGate(float sample);
};
