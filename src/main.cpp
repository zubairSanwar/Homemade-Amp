#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <limits>
#define MINIAUDIO_IMPLEMENTATION
#include "../external/miniaudio.h"

using namespace std;

struct Gain {
    atomic<float> new_gain{1.0}; //gain factor
    float current_gain=1.0; //current gain default at 1 -> for smooth adjustment
    atomic<float> smooth{0.002}; //smoothing coefficient to allow smooth transitiion to new set gain

    float applyGain(float sample){
        //move towards new gain logirithmically
        current_gain += smooth * (new_gain - current_gain);

        return sample*current_gain; //final gain used
    }
};

struct Distortion {
    atomic<float> new_drive{1.0};    //target drive
    float cur_drive=1.0;
    atomic<float> smooth{0.002};     //smooth coefficient for smooth transition to new set drive
    atomic<float> out_level{1.0};    

    float applyDistortion(float sample){  //apply distortion with smooth adjustment
        cur_drive += smooth * (new_drive - cur_drive);
        return out_level * tanh(cur_drive * sample);
    }

};

struct Loop {
    vector<float> loopBuf;
    int capacity;   //total sample slots in buffer (60 s)
    int loopLen;    //number of slots actually occupied with recording
    int pos;        //current position in buffer
    bool recording=false;   //initial recording -> sets loop length
    bool overdub=false;     //overdubbing on recording -> maintains length

    void init(int sampleRate){  //initialise loop buffer and indices
        loopBuf.assign(sampleRate*60,0);   //assign buffer of 60 seconds
        capacity = sampleRate*60;
        loopLen=0, pos=0;
    }

    void reset(){   //clear the loop by forgetting it
        loopLen=0;
        pos=0;
        recording=false;
        overdub=false;
    }

    void toggleRecord(){
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

    void toggleOverdub(){
        overdub = !overdub;
    }

    //The main thread only raises these; the audio thread below is what actually acts on
    //them. That keeps every change to recording/overdub/loopLen/pos on one thread, so
    //those stay plain variables and can never be seen half-updated.
    atomic<bool> recordReq{false};
    atomic<bool> overdubReq{false};
    atomic<bool> clearReq{false};

    void applyControls(){   //audio thread, once per buffer
        //exchange() reads the flag and clears it in one step, so a press is never
        //missed and never acted on twice
        if (clearReq.exchange(false))   reset();
        if (recordReq.exchange(false))  toggleRecord();
        if (overdubReq.exchange(false)) toggleOverdub();
    }

    float applyLoop(float sample){  //replay buffer (and optionally write to it) when called

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
};

struct Biquad {
    float b0=1,b1=0,b2=0,a1=0,a2=0;    //pass-through until the EQ fills in real coefficients
    float n1=0,n2=0;

    float applyBiquad(float sample){
        float y = b0*sample + n1;
        n1 = b1*sample - a1*y + n2; //store sample - 1
        n2 = b2*sample - a2*y;  //store sample - 2
        return y;
    }

    void setLowPass(float freq, float Q, float sr) {
        float w0 = 2.0f * 3.14159265f * freq / sr;
        float cosw = cosf(w0), alpha = sinf(w0) / (2.0f * Q);
        float a0 = 1.0f + alpha;
        b0 = ((1.0f - cosw) / 2.0f) / a0;
        b1 = (1.0f - cosw) / a0;
        b2 = b0;
        a1 = (-2.0f * cosw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setHighPass(float freq, float Q, float sr) {
        float w0 = 2.0f * 3.14159265f * freq / sr;
        float cosw = cosf(w0), alpha = sinf(w0) / (2.0f * Q);
        float a0 = 1.0f + alpha;
        b0 = ((1.0f + cosw) / 2.0f) / a0;
        b1 = (-(1.0f + cosw)) / a0;
        b2 = b0;
        a1 = (-2.0f * cosw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setLowShelf(float freq, float gainDb, float sr) {
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

    void setHighShelf(float freq, float gainDb, float sr) {
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

    void setPeaking(float freq, float gainDb, float Q, float sr) {
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
};

struct Delay {

    vector<float> delayBuf;
    int index=0, length;
    atomic<float> delay_t{0.3};    //delay time in seconds
    atomic<float> fade_factor{0.6};    //for delay to fade eventually
    int sample_rate;

    void init(int sampleRate){  //to initialise and reset delay
        delayBuf.assign(sampleRate*2,0);   //assign buffer of 2 seconds max
        length = sampleRate*2;
        sample_rate = sampleRate;
    }

    void resetDelay(){  //reset buffer to 0's to kill any echos
        fill(delayBuf.begin(), delayBuf.end(), 0.0f);
        index = 0;
    }

    atomic<bool> clearReq{false};   //raised by the main thread, acted on by the audio thread

    void applyControls(){   //audio thread, once per buffer
        //clearing 2 s of buffer is ~96k floats, about 40 us - well inside the 2.7 ms
        //deadline, and it must happen here rather than on the main thread because the
        //callback is reading this very buffer
        if (clearReq.exchange(false)) resetDelay();
    }

    float applyDelay(float sample){

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

};

struct EQ {
    Biquad low, mid, high;

    atomic<float> lowDb{0.0f}, midDb{0.0f}, highDb{0.0f};
    atomic<bool> dirty{true};
    float sr = 48000.0f;

    void init(float sampleRate){
        sr = sampleRate;
        dirty = true;
        applyControls();    //build the initial (flat) coefficients
    }

    void applyControls(){   //audio thread, once per buffer
        if (!dirty.exchange(false)) return;
        low.setLowShelf(100.0f,  lowDb.load(),  sr);    //bass
        mid.setPeaking(600.0f,   midDb.load(),  0.8f, sr);  //mids, wide-ish Q
        high.setHighShelf(3000.0f, highDb.load(), sr);  //treble
    }

    float applyEQ(float sample){
        return high.applyBiquad(mid.applyBiquad(low.applyBiquad(sample)));
    }
};

struct Gate {
    //kill singal when not playing to stop buzz
    atomic<float> threshold{0.00316f};  //-50 dBFS

    float env = 0.0f;       
    float g   = 0.0f;       //current multiplier: 0 = shut, 1 = fully open
    float attack=0.0f, release=0.0f, envFall=0.0f;

    void init(float sampleRate){
        attack  = 1.0f - expf(-1.0f/(0.001f*sampleRate));   //1 ms open: fast, so picking survives
        release = 1.0f - expf(-1.0f/(0.080f*sampleRate));   //80 ms close: slow, so notes ring out
        envFall = 1.0f - expf(-1.0f/(0.030f*sampleRate));   //how fast the level detector falls
        env = 0.0f; g = 0.0f;
    }

    float applyGate(float sample){
        float a = fabsf(sample);
        if (a > env) env = a;
        else env += envFall * (a - env);

        float t = threshold.load();
        float target = (env > t) ? 1.0f : ((env < t*0.5f) ? 0.0f : g);

        g += ((target > g) ? attack : release) * (target - g);
        return sample * g;
    }
};

struct Cabinet{
    Biquad highPass, lowPass1, lowPass2, bump;

    void init(float sampleRate) {
        highPass.setHighPass(90.0f, 0.707f, sampleRate);
        lowPass1.setLowPass(5000.0f, 0.707f, sampleRate);
        lowPass2.setLowPass(5000.0f, 0.707f, sampleRate);
        bump.setPeaking(2500.0f, 4.0f, 1.0f, sampleRate);
    }

    float applyCabinet(float sample) {
        float x = highPass.applyBiquad(sample);
        x = lowPass1.applyBiquad(x);
        x = lowPass2.applyBiquad(x);
        return bump.applyBiquad(x);
    }
};

struct Reverb {
    //Freeverb by Jezar at Dreampoint (public domain): 8 parallel damped
    //comb filters build the tail, 4 series allpasses smear it.

    atomic<float> roomSize{0.5f}, mix{0.3f}, damp{0.4f};

    std::vector<float> combBuf[8];
    int   combIdx[8]   = {};
    float combStore[8] = {};

    std::vector<float> apBuf[4];
    int apIdx[4] = {};

    void init(float sr) {
        const int combLen[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        const int apLen[4]   = {556, 441, 341, 225};
        float scale = sr / 44100.0f;
        for (int i = 0; i < 8; i++) { combBuf[i].assign((int)(combLen[i] * scale), 0.0f); combIdx[i] = 0; combStore[i] = 0.0f; }
        for (int i = 0; i < 4; i++) { apBuf[i].assign((int)(apLen[i] * scale), 0.0f);   apIdx[i] = 0; }
    }

    float applyReverb(float x) {
        float d = damp;
        float feedback = 0.7f + roomSize * 0.28f;
        float in = x * 0.015f;
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
        return x * sqrtf(1.0f - m) + wet * 3.5f * sqrtf(m);
    }
};

struct Amp{
    Gain gain;
    Distortion distortion;
    Loop loop;
    Delay delay;
    Cabinet cabinet;
    EQ eq;
    Gate gate;
    Reverb reverb;
    int sampleRate;
    atomic<float> master{1.0f};

    atomic<bool> distortion_on{false}, loop_on{false}, delay_on{false}, reverb_on{false}, cabinet_on{true}, eq_on{true}, gate_on{true};

    atomic<float> peakIn{0.0f}, peakOut{0.0f};

    //Which input channel the guitar is plugged into, counting from 0. The M-Audio
    //puts its instrument input on channel 2, so 1. A mono device (laptop mic) only
    //has channel 0 - the callback clamps this, so an out-of-range value is harmless.
    atomic<int> inputChannel{1};
    atomic<bool> testTone{false};   //DIAGNOSTIC: ignore the guitar, emit a sine instead

    void init(int sRate){
        sampleRate = sRate;

        loop.init(sampleRate);
        delay.init(sampleRate);
        cabinet.init(sampleRate);   
        eq.init(sampleRate);
        gate.init(sampleRate);
        reverb.init(sampleRate);
    }

    //adjusting internal values of each effect
    void adjustGain(float g){
        if(g>20.0f) g=20.0f;
        gain.new_gain = g;
    }
    void adjustDrive(float d){
        if (d<1.0f) d=1.0f;
        if (d>30.0f) d=30.0f;
        distortion.new_drive = d;
    }
    void adjustEffectSmoothing(float s){
        if(s<0) s=0.0f;
        if(s>1.0f) s=1.0f;
        gain.smooth = s;
        distortion.smooth = s;
    }
    void adjustDistortionLevel(float l){
        if(l<0.0f) l = 0.0f;
        if(l>1.0f) l = 1.0f;
        distortion.out_level = l;
    }
    void adjustDelayTime(float t){
        if (t>2.0f) t=2.0f;
        if (t<0.1f) t=0.1f;
        delay.delay_t = t;
    }
    void adjustEQLow(float db){
        if (db<-15.0f) db=-15.0f;
        if (db> 15.0f) db= 15.0f;
        eq.lowDb = db; eq.dirty = true;
    }
    void adjustEQMid(float db){
        if (db<-15.0f) db=-15.0f;
        if (db> 15.0f) db= 15.0f;
        eq.midDb = db; eq.dirty = true;
    }
    void adjustEQHigh(float db){
        if (db<-15.0f) db=-15.0f;
        if (db> 15.0f) db= 15.0f;
        eq.highDb = db; eq.dirty = true;
    }
    void adjustGateThreshold(float db){
        if (db<-90.0f) db=-90.0f;
        if (db>  0.0f) db=  0.0f;
        gate.threshold = powf(10.0f, db/20.0f);
    }
    void adjustDelayFadeFactor(float f){
        if (f>0.95f) f = 0.95f;
        if (f<0.0f) f = 0.0f;
        delay.fade_factor = f;
    }
    void adjustMaster(float v){
        if(v>1.0f) v=1.0f;
        if(v<0.0f) v=0.0f;
        master = v;
    }
    void adjustRoomSize(float rm){
        if(rm>0.9f) rm=0.9f;
        if(rm<0.0f) rm=0.0f;
        reverb.roomSize = rm;
    }
    void adjustReverbMix(float m){
        if(m>1.0f) m = 1.0f;
        if(m<0.0f) m = 0.0f;
        reverb.mix = m;
    }
    void adjustReverbDamp(float d){   //0 = bright tail, 1 = dark tail
        if(d>0.95f) d = 0.95f;
        if(d<0.0f)  d = 0.0f;
        reverb.damp = d;
    }
    
    
    void toggleLoopRecord(){
        loop.recordReq = true;
    }
    void toggleLoopOverdub(){
        loop.overdubReq = true;
    }

    //setting effects on / off
    void flipDistortion(){
        distortion_on=!distortion_on;
    }
    void flipLoop(){
        loop_on=!loop_on;
    }
    void flipDelay(){
        delay_on=!delay_on;
    }
    void flipCabinet(){
        cabinet_on=!cabinet_on;
    }
    void flipEQ(){
        eq_on=!eq_on;
    }
    void flipGate(){
        gate_on=!gate_on;
    }
    void flipReverb(){
        reverb_on=!reverb_on;
    }

    //resetting memory effects
    void resetLoop(){
        loop.clearReq = true;
    }
    void resetDelay(){
        delay.clearReq = true;
    }

    //audio thread: pick up anything the main thread asked for, once per buffer
    void applyControls(){
        loop.applyControls();
        delay.applyControls();
        eq.applyControls();     //rebuild filter coefficients if a tone knob moved
    }

    //applying effects depending on whether active or not
    float processAudio(float sample){
        float op = sample;
        
        if (gate_on) op = gate.applyGate(op);   //gate first to remove hissing -> clean sound
        if (loop_on) op = loop.applyLoop(op);   //loop is just clean guitar sound so comes first
        op = gain.applyGain(op);
        if (distortion_on) op=distortion.applyDistortion(op);
        if (eq_on) op = eq.applyEQ(op); //tone adjusted post distortion
        if (cabinet_on) op = cabinet.applyCabinet(op);   //the speaker effect
        if (delay_on) op=delay.applyDelay(op);
        if (reverb_on) op=reverb.applyReverb(op);   //at end so we echo final sound

        return op*master;   //apply master volume
    }


};

void audio_process(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    
    Amp* amp = (Amp*)pDevice->pUserData;

    float* out = (float*)pOutput;
    const float* in = (const float*)pInput;

    amp->applyControls();   //apply any knob presses once here, not per sample

    //The device picks its own channel counts, so read them rather than assume.
    //Both are fixed at init time, so they are safe to read from the audio thread.
    const ma_uint32 inCh  = pDevice->capture.channels;
    const ma_uint32 outCh = pDevice->playback.channels;

    //pick the guitar's channel once per buffer, clamped so we can never read past
    //the end of a frame no matter what the user sets
    ma_uint32 ch = (ma_uint32)amp->inputChannel.load();
    if (ch >= inCh) ch = inCh - 1;

    static float phase = 0.0f;
    bool tone = amp->testTone.load();
    float pin = 0.0f, pout = 0.0f;

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        //interleaved: frame i starts at i*inCh, then step to the channel we want
        float src = in[i * inCh + ch];
        if (fabsf(src) > pin) pin = fabsf(src);

        if (tone) {     //DIAGNOSTIC: quiet 440 Hz sine in place of the guitar
            src = 0.1f * sinf(phase);
            phase += 2.0f * 3.14159265f * 440.0f / amp->sampleRate;
            if (phase > 2.0f*3.14159265f) phase -= 2.0f*3.14159265f;
        }

        float op = amp->processAudio(src);

        //clipping input between fixed range to prevent issues in effects (e.g. in delay we might record faulty value leading to noise until reset)
        if (!isfinite(op)) op = 0.0f;
        if (op >  1.0f) op =  1.0f;
        if (op < -1.0f) op = -1.0f;

        if (fabsf(op) > pout) pout = fabsf(op);

        //mono out to however many output channels the device has (same signal to each)
        for (ma_uint32 c = 0; c < outCh; ++c) out[i * outCh + c] = op;
    }

    if (pin  > amp->peakIn.load())  amp->peakIn.store(pin);
    if (pout > amp->peakOut.load()) amp->peakOut.store(pout);
}

void handle_inputs(Amp* amp){   //allow control knobs through cmd line -> for testing
    cout << "Adjust: <cmd> <value>    Toggle/reset: <cmd>\n"
         << "  values   -> Gain: g, Drive: dr, Dist level: d, Smooth: s, Delay time: dt, Delay fade: df, Master: m\n"
         << "  switches -> Distortion: ds, Delay: dl, Loop: l, Cabinet: cb, EQ: eq, Gate: gs\n"
         << "  tone     -> Bass: eb <dB>, Mid: em <dB>, Treble: et <dB>  (-15..+15)\n"
         << "  gate     -> Threshold: gt <dBFS>  (try -50)\n"
         << "  reverb   -> Reverb: rev, Reverb Mix: revm <val>, Room Size: rrs <val>, Damping: rvd <val>\n"
         << "  looper   -> Record: r, Overdub: o, Reset loop: lr, Reset delay: del\n"
         << "  diag     -> Peak levels: pk, Test tone: t, Input channel: ic <n>\n"
         << "  quit     -> q\n\n";

    string cmd;
    float val;
    while (true) {
        cout << "CMD: " << flush;
        if (!(cin >> cmd)) break;   //end of input / ctrl-D

        //commands that expect a number after them
        bool needsVal = (cmd=="g"||cmd=="dr"||cmd=="d"||cmd=="s"||cmd=="dt"||cmd=="df"||cmd=="ic"
                      ||cmd=="eb"||cmd=="em"||cmd=="et"||cmd=="gt"||cmd=="m"||cmd=="revm"||cmd=="rrs"||cmd=="rvd");
        if (needsVal && !(cin >> val)) {
            //a failed read latches the stream into a failed state and every later read
            //returns false, so without this one typo would silently end the session
            cin.clear();                                           //drop the error flag
            cin.ignore(numeric_limits<streamsize>::max(), '\n');   //bin the rest of the line
            cout << "  need a number, e.g. " << cmd << " 1.5\n";
            continue;
        }

        if      (cmd=="q")   break;
        else if (cmd=="rrs") amp->adjustRoomSize(val);
        else if (cmd=="revm") amp->adjustReverbMix(val);
        else if (cmd=="rvd") amp->adjustReverbDamp(val);
        else if (cmd=="m") amp->adjustMaster(val);
        else if (cmd=="g")   amp->adjustGain(val);
        else if (cmd=="dr")  amp->adjustDrive(val);
        else if (cmd=="d")   amp->adjustDistortionLevel(val);
        else if (cmd=="s")   amp->adjustEffectSmoothing(val);
        else if (cmd=="dt")  amp->adjustDelayTime(val);
        else if (cmd=="df")  amp->adjustDelayFadeFactor(val);
        else if (cmd=="ds")  amp->flipDistortion();
        else if (cmd=="dl")  amp->flipDelay();
        else if (cmd=="l")   amp->flipLoop();
        else if (cmd=="cb")  amp->flipCabinet();
        else if (cmd=="eq")  amp->flipEQ();
        else if (cmd=="gs")  amp->flipGate();
        else if (cmd=="rev") amp->flipReverb();
        else if (cmd=="eb")  amp->adjustEQLow(val);
        else if (cmd=="em")  amp->adjustEQMid(val);
        else if (cmd=="et")  amp->adjustEQHigh(val);
        else if (cmd=="gt")  amp->adjustGateThreshold(val);
        else if (cmd=="r")   amp->toggleLoopRecord();
        else if (cmd=="o")   amp->toggleLoopOverdub();
        else if (cmd=="lr")  amp->resetLoop();
        else if (cmd=="del") amp->resetDelay();
        else if (cmd=="pk") {   //DIAGNOSTIC: peak since last check - is audio flowing?
            cout << "  peak in=" << amp->peakIn.exchange(0.0f)
                 << "  out=" << amp->peakOut.exchange(0.0f) << "\n";
        }
        else if (cmd=="ic") {   //which input channel the guitar is on (0-based)
            amp->inputChannel = (int)val;
            cout << "  input channel " << (int)val << "\n";
        }
        else if (cmd=="t") {    //DIAGNOSTIC: test tone, takes the guitar out of the picture
            bool on = !amp->testTone.load();
            amp->testTone = on;
            cout << "  test tone " << (on?"ON":"off") << "\n";
        }
        else cout << "? unknown command\n";
    }
}

int main() {

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);

    //Format we want miniaudio to hand us: f32 is a conversion it does for free.
    //Channels and sample rate stay 0 = "whatever the device natively uses". Forcing
    //capture to 1 channel on a 2-channel interface makes CoreAudio hand back a silent
    //stream instead of downmixing, which is why the guitar never arrived.
    config.capture.format   = ma_format_f32;   // float samples in
    config.capture.channels = 0;               // 0 = native
    config.playback.format  = ma_format_f32;   // float samples out
    config.playback.channels = 0;              // 0 = native
    config.sampleRate       = 0;               // 0 = native
    config.periodSizeInFrames = 128;           // requested buffer size
    config.dataCallback     = audio_process;   // <-- handing it over 

    Amp amp;
    config.pUserData = &amp;    //pass amp to the callback

    ma_device device;
    if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
        cout << "Failed to initialize audio device\n";
        return -1;
    }

    // // ---- TEMPORARY DIAGNOSTIC ----
    // cout << "capture : " << device.capture.name
    //     << "  ch=" << device.capture.internalChannels
    //     << " rate=" << device.capture.internalSampleRate
    //     << " period=" << device.capture.internalPeriodSizeInFrames
    //     << "   [app sees " << device.capture.channels << " ch]\n";
    // cout << "playback: " << device.playback.name
    //     << "  ch=" << device.playback.internalChannels
    //     << " rate=" << device.playback.internalSampleRate
    //     << " period=" << device.playback.internalPeriodSizeInFrames
    //     << "   [app sees " << device.playback.channels << " ch]\n";
    // // ------------------------------


    amp.init(device.sampleRate);
    cout << "Sample rate: " << device.sampleRate << "\n";
    cout << "Audio running.\n\n";

    // from here, miniaudio calls audio_process repeatedly
    if (ma_device_start(&device) != MA_SUCCESS) {
        cout << "Failed to start audio device\n";
        ma_device_uninit(&device);
        return -1;
    }
    handle_inputs(&amp);        // keeps main alive; the audio runs on another thread

    ma_device_uninit(&device);
    return 0;
}