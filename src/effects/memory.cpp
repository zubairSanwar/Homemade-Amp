/*
IMPLEMENTATION FOR MEMORY RELATED EFFECTS REQUIRING BUFFERS.

Includes:
- Delay
- Loop
- Reverb
*/
#include "memory.h"
#include <algorithm>
#include <cmath>

void Loop::init(int sampleRate){  //initialise loop buffer and indices
    loopBuf.assign(sampleRate*60,0);   //assign buffer of 60 seconds
    capacity = sampleRate*60;
    loopLen=0, pos=0;
}

void Loop::reset(){   //clear the loop by forgetting it
    loopLen=0;
    pos=0;
    recording=false;
    overdub=false;
}

void Loop::toggleRecord(){
    if (recording){         //second press closes the loop at whatever we just played
        loopLen = pos;
        pos = 0;
        recording = false;
    }
    else if (loopLen==0){   //only the first pass is allowed to set the length
        pos = 0;
        recording = true;
    }
}

void Loop::toggleOverdub(){
    overdub = !overdub;
}

void Loop::applyControls(){   //audio thread, once per buffer
    //exchange() reads the flag and clears it in one step, so a press is never
    //missed and never acted on twice
    if (clearReq.exchange(false))   reset();
    if (recordReq.exchange(false))  toggleRecord();
    if (overdubReq.exchange(false)) toggleOverdub();
}

float Loop::applyLoop(float sample){  //replay buffer (and optionally write to it) when called

    if (loopLen==0){    //nothing recorded yet, so the only thing we can do is lay down initial recording
        if (recording){
            loopBuf[pos] = sample;  //the looper stores the DRY signal
            pos++;
            if (pos>=capacity){ //buffer full -> close loop
                loopLen = capacity;
                pos = 0;
                recording = false;
            }
        }
        return sample;
    }

    if (pos>=loopLen) pos = 0;      //wrap at the end of the recorded part, not the whole buffer
    float op = sample + loopBuf[pos];
    if (overdub) loopBuf[pos] = op; //layer the live playing onto what is already there
    pos++;

    return op;
}

void Delay::init(int sampleRate){  //to initialise and reset delay
    delayBuf.assign(sampleRate*2,0);   //assign buffer of 2 seconds max
    length = sampleRate*2;
    sample_rate = sampleRate;
}

void Delay::resetDelay(){  //reset buffer to 0's to kill any echos
    std::fill(delayBuf.begin(), delayBuf.end(), 0.0f);
    index = 0;
}

void Delay::applyControls(){   //audio thread, once per buffer
    //clearing 2 s of buffer is ~96k floats, about 40 us - well inside the 2.7 ms
    //deadline, and it must happen here rather than on the main thread because the
    //callback is reading this very buffer
    if (clearReq.exchange(false)) resetDelay();
}

float Delay::applyDelay(float sample){

    //find position to read for delay echo
    int offset = (int)(sample_rate*delay_t);
    if (offset<1) offset = 1;                   //0 would read the slot we are about to overwrite
    if (offset>length-1) offset = length-1;     //never ask for more delay than the buffer holds

    int read_index = index - offset;
    if (read_index<0){  //if below 0, wrap around
        read_index+=length;
    }

    delayBuf[index] = sample + (delayBuf[read_index]*fade_factor);  //record current timestep event
    float op = delayBuf[index]; //save return val

    //update index
    index++;
    if (index>=length) index=0;
    return op;
}

void Reverb::init(float sr) {
    const int combLen[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    const int apLen[4]   = {556, 441, 341, 225};
    float scale = sr / 44100.0f;
    for (int i = 0; i < 8; i++) { combBuf[i].assign((int)(combLen[i] * scale), 0.0f); combIdx[i] = 0; combStore[i] = 0.0f; }
    for (int i = 0; i < 4; i++) { apBuf[i].assign((int)(apLen[i] * scale), 0.0f);   apIdx[i] = 0; }
}

float Reverb::applyReverb(float sample) {
    float d = damp;
    float feedback = 0.7f + roomSize * 0.28f;
    float in = sample * 0.015f;
    float wet = 0;

    for (int i = 0; i < 8; i++) {
        float out = combBuf[i][combIdx[i]];
        combStore[i] = out * (1.0f - d) + combStore[i] * d;
        combBuf[i][combIdx[i]] = in + combStore[i] * feedback;
        if (++combIdx[i] >= (int)combBuf[i].size()) combIdx[i] = 0;
        wet += out;
    }

    for (int i = 0; i < 4; i++) {
        float delayed = apBuf[i][apIdx[i]];
        apBuf[i][apIdx[i]] = wet + delayed * 0.5f;
        if (++apIdx[i] >= (int)apBuf[i].size()) apIdx[i] = 0;
        wet = -wet + delayed;
    }

    //equal-power crossfade: dry and wet are uncorrelated, so fading them with
    //sqrt keeps the total level flat. A plain (1-mix)/mix fade dips ~3 dB in
    //the middle. 3.5 is the gain that puts the fully-wet signal at unity.
    float m = mix;
    return sample * sqrtf(1.0f - m) + wet * 3.5f * sqrtf(m);
}
