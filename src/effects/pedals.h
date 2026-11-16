/*
HEADER FOR PEDAL EFFECTS (NOT USUALLY INCLUDED IN STANDARD AMP)

Includes:
- Wah
- Tremolo
*/
#pragma once

#include "tone.h"
#include <atomic>

struct Wah{
    Biquad peak;
    std::atomic<float> sensitivity{6.0f}, resonance{3.0f};
    float env=0.0f;
    float curF = 400.0f;    
    float attack = 0.0f, release = 0.0f;
    float sr = 48000.0f;
    int counter = 0;        //coefficients are rebuilt every CTRL_STEP samples

    void init(float sampleRate);
    float applyWah(float sample);
};

struct Tremolo{
    std::atomic<float> rate{5.0f}, resonance{0.5f}; //rate of switch + how low we go
    float phase=0;  //where we are in the wave right now
    float sampleRate = 48000.0f;   //real value set by init(); default keeps it finite
    void init(float sr);
    float applyTremolo(float sample);
};