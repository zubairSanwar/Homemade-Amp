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

void Loop::init(int sRate){  //initialise loop buffer and indices
    sampleRate = sRate;
    loopBuf.assign(sRate*60,0);   //assign buffer of 60 seconds
    capacity = sRate*60;
    loopLen=0, pos=0;
    beepLen = (int)(0.025f*sRate);   //25 ms click - short and sharp, like a metronome

    //Start with a usable tempo instead of waiting to be tapped. The count-in and
    //the beat snapping were both gated on a tap, so hitting record before tapping
    //gave no ticks and no quantising at all - which looked like neither feature
    //existed. 120 BPM also makes sense musically: the count-in tells you the tempo,
    //so playing along with it is what makes snapping to it correct.
    beatSamples = (int)(sRate * 60.0f / 120.0f);
    bpm = 120.0f;
}

void Loop::reset(){   //clear the loop by forgetting it
    loopLen=0;
    pos=0;
    recording=false;
    overdub=false;
    countInLeft=0;      //abandon a count-in that was still running
    beepLeft=0;
    beepOut=0.0f;
}

//Each tap is one beat apart. We keep the last few gaps and average them
void Loop::registerTap(){
    if (lastTap >= 0){
        long long gap = clock - lastTap;

        //Anything outside 40..240 BPM is not a beat
        long long minGap = (long long)(sampleRate * 60.0f / 240.0f);
        long long maxGap = (long long)(sampleRate * 60.0f / 40.0f);

        if (gap >= minGap && gap <= maxGap){
            tapIntervals[tapCount % 4] = (int)gap;
            tapCount++;
            int n = (tapCount < 4) ? tapCount : 4;
            long long sum = 0;
            for (int i=0;i<n;i++) sum += tapIntervals[i];
            beatSamples = (int)(sum / n);
            bpm = 60.0f * sampleRate / (float)beatSamples;
        } else {
            tapCount = 0;   //out of range: this tap starts a fresh count
        }
    }
    lastTap = clock;
}

void Loop::startBeep(float freq){
    beepFreq  = freq;
    beepPhase = 0.0f;
    beepLeft  = beepLen;
}

void Loop::toggleRecord(){
    if (countInLeft > 0){   //pressed again mid count-in: cancel it
        countInLeft = 0;
        beepLeft = 0;
        return;
    }

    if (recording){
        //Snap the length to the nearest whole beat. Pressing stop is worth about +/-100 ms of human error, which leaves dead air 
        int wanted = pos;
        if (beatSamples > 0){
            int beats = (pos + beatSamples/2) / beatSamples;   //nearest, not floor
            if (beats < 1) beats = 1;                          //never a zero-length loop
            wanted = beats * beatSamples;
            if (wanted > capacity) wanted = capacity;
        }

        //Rounded up, so this pass never wrote the tail.
        for (int i = pos; i < wanted; i++) loopBuf[i] = 0.0f;

        //Playback jumps from loopBuf[len-1] straight back to loopBuf[0]. 
        int xf = (int)(0.010f*sampleRate);      //10 ms
        if (xf > wanted/4) xf = wanted/4;
        if (xf < 1) xf = 0;

        int overhang = pos - wanted;            //audio recorded past the loop end
        if (xf > 0 && overhang >= xf){
            //Stopped late, so we still have the natural continuation of the end
            for (int i=0;i<xf;i++){
                float w = (float)i/(float)xf;   //0 -> 1
                loopBuf[i] = loopBuf[wanted+i]*(1.0f-w) + loopBuf[i]*w;
            }
        } else if (xf > 0){
            //Stopped early: nothing recorded past the end, so take both sides to zero instead
            int tailEnd = (pos < wanted) ? pos : wanted;
            for (int i=0;i<xf;i++){
                float w = (float)i/(float)xf;
                loopBuf[i] *= w;                            //head fades in
                int t = tailEnd-1-i;
                if (t >= 0) loopBuf[t] *= w;                //tail fades out
            }
        }

        loopLen = wanted;
        pos = 0;
        recording = false;
    }
    else if (loopLen==0){   //only the first pass is allowed to set the length
        pos = 0;
        if (beatSamples > 0){
            countInLeft = 3 * beatSamples;   //3 ticks, then record on the downbeat
        } else {
            recording = true;                //no tempo tapped yet: start straight away
        }
    }
}

void Loop::toggleOverdub(){
    overdub = !overdub;
}

void Loop::applyControls(int frameCount){   //audio thread, once per buffer
    clock += frameCount;    //the only clock the looper has - samples, not seconds

    //exchange() reads the flag and clears it in one step, so a press is never
    //missed and never acted on twice
    if (clearReq.exchange(false))   reset();
    if (tapReq.exchange(false))     registerTap();
    if (recordReq.exchange(false))  toggleRecord();
    if (overdubReq.exchange(false)) toggleOverdub();
}

float Loop::applyLoop(float sample){  //replay buffer (and optionally write to it) when called

    //Runs before anything else so the clicks keep time even while nothing is being recorded
    beepOut = 0.0f;
    if (countInLeft > 0){
        if (countInLeft % beatSamples == 0) startBeep(1200.0f);   //one tick per beat
        countInLeft--;
        if (countInLeft == 0){          //downbeat: start recording, on a higher click
            recording = true;
            pos = 0;
            startBeep(2000.0f);   //downbeat: higher, so you know it has started
        }
    }
    if (beepLeft > 0){
        // metronome tick 
        float x   = 1.0f - (float)beepLeft/(float)beepLen;   //0 at onset, 1 at the end
        float env = expf(-6.0f*x);                           //~0.2% left by the end
        beepOut = 0.40f * env * (sinf(beepPhase) + 0.35f*sinf(2.0f*beepPhase));
        beepPhase += 2.0f*3.14159265f*beepFreq/(float)sampleRate;
        if (beepPhase > 2.0f*3.14159265f) beepPhase -= 2.0f*3.14159265f;
        beepLeft--;
    }

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
