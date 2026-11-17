/*
AMP HEADER WHICH CONNECTS ALL EFFECTS INTO A SINGLE AMP
*/
#pragma once

#include <atomic>
#include "../effects/drive.h"
#include "../effects/memory.h"
#include "../effects/tone.h"
#include "../effects/pedals.h"

struct Amp{
    //declaring effects
    Gain gain;
    Distortion distortion;
    Loop loop;
    Delay delay;
    Cabinet cabinet;
    EQ eq;
    NoiseGate gate;
    Reverb reverb;
    Wah wah;
    Tremolo tremolo;
    int sampleRate = 48000;

    std::atomic<float> master{1.0f};    //setting master volume

    //switches for drive effects
    std::atomic<bool> distortion_on{false}, gate_on{true};

    //switches for memory effects
    std::atomic<bool> loop_on{false}, delay_on{false}, reverb_on{false};

    //switches for tone effects
    std::atomic<bool> eq_on{true}, cabinet_on{true};

    //switches for pedal effects
    std::atomic<bool> wah_on{false}, tremolo_on{false};

    //How many input channels we check to find guitar channel
    static constexpr int MAX_METER_CH = 8;

    //Peak since the last `pk`, one per input channel plus one for the output
    std::atomic<float> peakCh[MAX_METER_CH] = {};
    std::atomic<float> peakOut{0.0f};
    int inputChannels = 1;

    //Which input channel the guitar is plugged into
    std::atomic<int> inputChannel{0};

    //initialise 
    void init(int sRate);

    //adjust knobs
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
    void adjustWahSensitivity(float s);
    void adjustWahResonance(float q);
    void adjustWahMix(float m);
    void adjustTremoloRate(float r);
    void adjustTremoloDepth(float d);

    //adjust on/off switches
    void toggleLoopRecord();
    void toggleLoopOverdub();
    void tapLoopTempo();
    void flipDistortion();
    void flipLoop();
    void flipDelay();
    void flipCabinet();
    void flipEQ();
    void flipGate();
    void flipReverb();
    void flipWah();
    void flipTremolo();

    //statefull effects toggling
    void setDistortion(bool on);
    void setLoop(bool on);
    void setDelay(bool on);
    void setCabinet(bool on);
    void setEQ(bool on);
    void setGate(bool on);
    void setReverb(bool on);
    void setWah(bool on);
    void setTremolo(bool on);

    //reset memory effects
    void resetLoop();
    void resetDelay();

    //set flags once and start processing
    void applyControls(int frameCount);
    float processAudio(float sample);
};
