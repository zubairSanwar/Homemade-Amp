/*
Includes:
- Gain
- Distortion
- Noise Gate
*/
#include "drive.h"
#include <cmath>

float Gain::applyGain(float sample){
    //move towards new gain logirithmically
    current_gain += smooth * (new_gain - current_gain);

    return sample*current_gain; //final gain used
}

float Distortion::applyDistortion(float sample){  //apply distortion with smooth adjustment
    cur_drive += smooth * (new_drive - cur_drive);
    return out_level * tanhf(cur_drive * sample);
}

void NoiseGate::init(float sampleRate){
    attack  = 1.0f - expf(-1.0f/(0.001f*sampleRate));   //1 ms open: fast, so picking survives
    release = 1.0f - expf(-1.0f/(0.080f*sampleRate));   //80 ms close: slow, so notes ring out
    envFall = 1.0f - expf(-1.0f/(0.030f*sampleRate));   //how fast the level detector falls
    env = 0.0f; g = 0.0f;
}

float NoiseGate::applyGate(float sample){
    float a = fabsf(sample);
    if (a > env) env = a;
    else env += envFall * (a - env);

    float t = threshold.load();
    float target = (env > t) ? 1.0f : ((env < t*0.5f) ? 0.0f : g);

    g += ((target > g) ? attack : release) * (target - g);
    return sample * g;
}
