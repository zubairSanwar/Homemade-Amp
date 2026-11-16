/*
HEADER FILE FOR ALL TONE EFFECTS.

Includes:
- Biquad helper struct
- EQ
- Cabinet Filter
*/
#pragma once

#include <atomic>

struct Biquad{
    float b0=1,b1=0,b2=0,a1=0,a2=0;    //pass-through until the EQ fills in real coefficients
    float n1=0,n2=0;

    float applyBiquad(float sample);
    void setLowPass(float freq, float Q, float sr);
    void setHighPass(float freq, float Q, float sr);
    void setLowShelf(float freq, float gainDb, float sr);
    void setHighShelf(float freq, float gainDb, float sr);
    void setPeaking(float freq, float gainDb, float Q, float sr);
};

struct EQ{
    Biquad low, mid, high;

    std::atomic<float> lowDb{0.0f}, midDb{0.0f}, highDb{0.0f};
    std::atomic<bool> dirty{true};
    float sr = 48000.0f;

    void init(float sampleRate);
    void applyControls();
    float applyEQ(float sample);
};

struct Cabinet{
    Biquad highPass, lowPass1, lowPass2, bump;

    void init(float sampleRate);
    float applyCabinet(float sample);
};
