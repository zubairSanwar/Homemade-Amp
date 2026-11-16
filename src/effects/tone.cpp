/*
IMPLEMENTATION OF TONAL EFFECTS.

Includes:
- Biquad helper struct
- EQ
- Cabinet Filter
*/

#include "tone.h"
#include <cmath>

float Biquad::applyBiquad(float sample){
    float y = b0*sample + n1;
    n1 = b1*sample - a1*y + n2; //store sample - 1
    n2 = b2*sample - a2*y;  //store sample - 2
    return y;
}

void Biquad::setLowPass(float freq, float Q, float sr) {
    float w0 = 2.0f * 3.14159265f * freq / sr;
    float cosw = cosf(w0), alpha = sinf(w0) / (2.0f * Q);
    float a0 = 1.0f + alpha;
    b0 = ((1.0f - cosw) / 2.0f) / a0;
    b1 = (1.0f - cosw) / a0;
    b2 = b0;
    a1 = (-2.0f * cosw) / a0;
    a2 = (1.0f - alpha) / a0;
}

void Biquad::setHighPass(float freq, float Q, float sr) {
    float w0 = 2.0f * 3.14159265f * freq / sr;
    float cosw = cosf(w0), alpha = sinf(w0) / (2.0f * Q);
    float a0 = 1.0f + alpha;
    b0 = ((1.0f + cosw) / 2.0f) / a0;
    b1 = (-(1.0f + cosw)) / a0;
    b2 = b0;
    a1 = (-2.0f * cosw) / a0;
    a2 = (1.0f - alpha) / a0;
}

void Biquad::setLowShelf(float freq, float gainDb, float sr) {
    float A = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * 3.14159265f * freq / sr;
    float cosw = cosf(w0);
    float alpha = sinf(w0) / 2.0f * 1.41421356f;   //S=1, the usual shelf slope
    float sqA2alpha = 2.0f * sqrtf(A) * alpha;
    float a0 =          (A+1.0f) + (A-1.0f)*cosw + sqA2alpha;
    b0 =        A * (   (A+1.0f) - (A-1.0f)*cosw + sqA2alpha ) / a0;
    b1 = 2.0f * A * (   (A-1.0f) - (A+1.0f)*cosw             ) / a0;
    b2 =        A * (   (A+1.0f) - (A-1.0f)*cosw - sqA2alpha ) / a0;
    a1 =    -2.0f * (   (A-1.0f) + (A+1.0f)*cosw             ) / a0;
    a2 =            (   (A+1.0f) + (A-1.0f)*cosw - sqA2alpha ) / a0;
}

void Biquad::setHighShelf(float freq, float gainDb, float sr) {
    float A = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * 3.14159265f * freq / sr;
    float cosw = cosf(w0);
    float alpha = sinf(w0) / 2.0f * 1.41421356f;
    float sqA2alpha = 2.0f * sqrtf(A) * alpha;
    float a0 =          (A+1.0f) - (A-1.0f)*cosw + sqA2alpha;
    b0 =        A * (   (A+1.0f) + (A-1.0f)*cosw + sqA2alpha ) / a0;
    b1 =-2.0f * A * (   (A-1.0f) + (A+1.0f)*cosw             ) / a0;
    b2 =        A * (   (A+1.0f) + (A-1.0f)*cosw - sqA2alpha ) / a0;
    a1 =     2.0f * (   (A-1.0f) - (A+1.0f)*cosw             ) / a0;
    a2 =            (   (A+1.0f) - (A-1.0f)*cosw - sqA2alpha ) / a0;
}

void Biquad::setPeaking(float freq, float gainDb, float Q, float sr) {
    float A = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * 3.14159265f * freq / sr;
    float cosw = cosf(w0), alpha = sinf(w0) / (2.0f * Q);
    float a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0;
    b1 = (-2.0f * cosw) / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = b1;  // same -2*cos(w0) term, same a0 division
    a2 = (1.0f - alpha / A) / a0;
}

void EQ::init(float sampleRate){
    sr = sampleRate;
    dirty = true;
    applyControls();    //build the initial (flat) coefficients
}

void EQ::applyControls(){   //audio thread, once per buffer
    if (!dirty.exchange(false)) return;
    low.setLowShelf(100.0f,  lowDb.load(),  sr);    //bass
    mid.setPeaking(600.0f,   midDb.load(),  0.8f, sr);  //mids, wide-ish Q
    high.setHighShelf(3000.0f, highDb.load(), sr);  //treble
}

float EQ::applyEQ(float sample){
    return high.applyBiquad(mid.applyBiquad(low.applyBiquad(sample)));
}

void Cabinet::init(float sampleRate) {
    highPass.setHighPass(90.0f, 0.707f, sampleRate);
    lowPass1.setLowPass(5000.0f, 0.707f, sampleRate);
    lowPass2.setLowPass(5000.0f, 0.707f, sampleRate);
    bump.setPeaking(2500.0f, 4.0f, 1.0f, sampleRate);
}

float Cabinet::applyCabinet(float sample) {
    float x = highPass.applyBiquad(sample);
    x = lowPass1.applyBiquad(x);
    x = lowPass2.applyBiquad(x);
    return bump.applyBiquad(x);
}
