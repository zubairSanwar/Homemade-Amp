/*
HEADER FOR MEMORY RELATED EFFECTS REQUIRING BUFFERS.

Includes:
- Delay
- Loop
- Reverb
*/
#pragma once

#include <vector>
#include <atomic>

struct Loop{
    std::vector<float> loopBuf;
    int capacity = 0;   //total sample slots in buffer (60 s)
    int loopLen  = 0;   //number of slots actually occupied with recording
    int pos      = 0;   //current position in buffer
    bool recording = false; //initial recording -> sets loop length
    bool overdub   = false; //overdubbing on recording -> maintains length

    std::atomic<bool> recordReq{false}, overdubReq{false}, clearReq{false}, tapReq{false};

    int sampleRate = 48000;

    long long clock   = 0;      //samples processed since init
    long long lastTap = -1;     //clock at the previous tap, -1 = none yet
    int tapIntervals[4] = {};   //the last few gaps, in samples
    int tapCount    = 0;
    int beatSamples = 0;        //set to 120 BPM by init(); tapping replaces it
    std::atomic<float> bpm{0.0f};   //published for the UI; init() sets 120

    int   countInLeft = 0;      //samples left before recording begins, 0 = not counting
    int   beepLeft    = 0;      //samples left of the current click
    int   beepLen     = 0;
    float beepPhase   = 0.0f;
    float beepFreq    = 0.0f;
    float beepOut     = 0.0f;   //this sample's click - Amp adds it AFTER master so it
                                //never runs through distortion, cabinet or reverb

    void init(int sampleRate);
    void reset();
    void toggleRecord();
    void toggleOverdub();
    void registerTap();
    void startBeep(float freq);
    void applyControls(int frameCount);
    float applyLoop(float sample);
};

struct Delay{
    std::vector<float> delayBuf;
    int index = 0, length = 0, sample_rate = 0;
    std::atomic<float> delay_t{0.3f};       //delay time in seconds
    std::atomic<float> fade_factor{0.6f};   //for delay to fade eventually
    std::atomic<bool> clearReq{false};      //raised by the main thread, acted on by the audio thread

    void init(int sampleRate);
    void resetDelay();
    void applyControls();
    float applyDelay(float sample);
};

struct Reverb{
    /*
    CREDIT:
        Freeverb by Jezar at Dreampoint (public domain): 8 parallel damped
        comb filters build the tail, 4 series allpasses smear it.
    */
    std::atomic<float> roomSize{0.5f}, mix{0.3f}, damp{0.4f};

    std::vector<float> combBuf[8];
    int   combIdx[8]   = {};
    float combStore[8] = {};

    std::vector<float> apBuf[4];
    int apIdx[4] = {};

    void init(float sr);
    float applyReverb(float sample);
};
