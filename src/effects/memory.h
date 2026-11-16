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

    std::atomic<bool> recordReq{false}, overdubReq{false}, clearReq{false};

    void init(int sampleRate);
    void reset();
    void toggleRecord();
    void toggleOverdub();
    void applyControls();
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
