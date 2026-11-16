/*
AMP HEADER WHICH CONNECTS ALL EFFECTS INTO A SINGLE AMP
*/
#pragma once

#include <atomic>
#include "../effects/drive.h"
#include "../effects/memory.h"
#include "../effects/tone.h"

struct Amp{
    Gain gain;
    Distortion distortion;
    Loop loop;
    Delay delay;
    Cabinet cabinet;
    EQ eq;
    NoiseGate gate;
    Reverb reverb;
    int sampleRate = 48000;
    std::atomic<float> master{1.0f};

    std::atomic<bool> distortion_on{false}, loop_on{false}, delay_on{false}, reverb_on{false}, cabinet_on{true}, eq_on{true}, gate_on{true};

    std::atomic<float> peakIn{0.0f}, peakOut{0.0f};

    //Which input channel the guitar is plugged into, counting from 0. The M-Audio
    //puts its instrument input on channel 2, so 1. A mono device (laptop mic) only
    //has channel 0 - the callback clamps this, so an out-of-range value is harmless.
    std::atomic<int> inputChannel{1};
    std::atomic<bool> testTone{false};   //DIAGNOSTIC: ignore the guitar, emit a sine instead

    void init(int sRate);

    void adjustGain(float g);
    void adjustDrive(float d);
    void adjustEffectSmoothing(float s);
    void adjustDistortionLevel(float l);
    void adjustDelayTime(float t);
    void adjustEQLow(float db);
    void adjustEQMid(float db);
    void adjustEQHigh(float db);
    void adjustGateThreshold(float db);
    void adjustDelayFadeFactor(float f);
    void adjustMaster(float v);
    void adjustRoomSize(float rm);
    void adjustReverbMix(float m);
    void adjustReverbDamp(float d);

    void toggleLoopRecord();
    void toggleLoopOverdub();
    void flipDistortion();
    void flipLoop();
    void flipDelay();
    void flipCabinet();
    void flipEQ();
    void flipGate();
    void flipReverb();

    void resetLoop();
    void resetDelay();

    void applyControls();
    float processAudio(float sample);
};
