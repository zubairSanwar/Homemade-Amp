/*
IMPLEMENTATIONS OF PEDAL EFFECTS.

Includes:
- Wah
- Tremolo
*/

#include "pedals.h"
#include <cmath>

void Wah::init(float sampleRate){
    sr = sampleRate;
    attack  = 1.0f - expf(-1.0f/(0.005f*sr));   //5 ms: snaps open on the pick
    release = 1.0f - expf(-1.0f/(0.150f*sr));   //150 ms: slides back down
    env = 0.0f;
    curF = 400.0f;
    counter = 0;
    peak.setPeaking(curF, 12.0f, resonance.load(), sr);
};

float Wah::applyWah(float sample){

    //essentially a high and low pass filter switcing at a fixed rate

    float a=fabsf(sample);  //track how loud current play
    env += ((a > env) ? attack : release) * (a - env);

    if (++counter >= 32){
        counter = 0;

        float drive = env * sensitivity;
        if (drive > 1.0f) drive = 1.0f;
        float targetF = 400.0f * powf(2200.0f/400.0f, drive);

        curF += 0.15f * (targetF - curF);   
        peak.setPeaking(curF, 12.0f, resonance.load(), sr);
    }

    return peak.applyBiquad(sample);

};

void Tremolo::init(float sr){
    sampleRate = sr;
}

float Tremolo::applyTremolo(float sample){
    //switching volume at a fixed rate following sin wave
    float lfo = 0.5f + 0.5f * sinf(2.0f * 3.14159265f * phase);  // 0..1
    float gain = 1.0f - resonance * (1.0f - lfo);                    // (1-resonance)..1
    phase += rate / sampleRate;
    if (phase >= 1.0f) phase -= 1.0f;
    return sample * gain;
}