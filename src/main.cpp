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
        n1 = b1*sample - a1*y + n2;
        n2 = b2*sample - a2*y;
        return y;
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

struct Amp{
    Gain gain;
    Distortion distortion;
    Loop loop;
    Delay delay;
    int sampleRate;

    //written by the main thread (the flip functions), read by the audio thread every
    //sample, so they have to be atomic or the optimiser may never reload them
    atomic<bool> distortion_on{false}, loop_on{false}, delay_on{false};

    void init(int sRate){
        sampleRate = sRate;

        loop.init(sampleRate);
        delay.init(sampleRate);
        distortion_on=false;
        loop_on=false;
        delay_on=false;
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
    void adjustDelayFadeFactor(float f){
        //needs to be between 0-0.95 -> if higher then delay may never fade leading to noise
        if (f>0.95f) f = 0.95f;
        if (f<0.0f) f = 0.0f;
        delay.fade_factor = f;
    }
    //these four are called from the main thread, so they only ask - see applyControls
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
    }

    //applying effects depending on whether active or not
    float processAudio(float sample){
        float op = sample;
        if (loop_on) op = loop.applyLoop(sample);
        op = gain.applyGain(op);
        if (distortion_on) op=distortion.applyDistortion(op);
        if (delay_on) op=delay.applyDelay(op);

        return op;
    }


};

void audio_process(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    
    Amp* amp = (Amp*)pDevice->pUserData;

    float* out = (float*)pOutput;
    const float* in = (const float*)pInput;

    amp->applyControls();   //apply any knob presses once here, not per sample

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        float op = amp->processAudio(in[i]);

        //clipping input between fixed range to prevent issues in effects (e.g. in delay we might record faulty value leading to noise until reset)
        if (!isfinite(op)) op = 0.0f;
        if (op >  1.0f) op =  1.0f;
        if (op < -1.0f) op = -1.0f;

        //mono to stereo
        out[i * 2]     = op;   
        out[i * 2 + 1] = op;   
    }
}

void handle_inputs(Amp* amp){   //allow control knobs through cmd line -> for testing
    cout << "Adjust: <cmd> <value>    Toggle/reset: <cmd>\n"
         << "  values   -> Gain: g, Drive: dr, Dist level: d, Smooth: s, Delay time: dt, Delay fade: df\n"
         << "  switches -> Distortion: ds, Delay: dl, Loop: l\n"
         << "  looper   -> Record: r, Overdub: o, Reset loop: lr, Reset delay: del\n"
         << "  quit     -> q\n\n";

    string cmd;
    float val;
    while (true) {
        cout << "CMD: " << flush;
        if (!(cin >> cmd)) break;   //end of input / ctrl-D

        //commands that expect a number after them
        bool needsVal = (cmd=="g"||cmd=="dr"||cmd=="d"||cmd=="s"||cmd=="dt"||cmd=="df");
        if (needsVal && !(cin >> val)) {
            //a failed read latches the stream into a failed state and every later read
            //returns false, so without this one typo would silently end the session
            cin.clear();                                           //drop the error flag
            cin.ignore(numeric_limits<streamsize>::max(), '\n');   //bin the rest of the line
            cout << "  need a number, e.g. " << cmd << " 1.5\n";
            continue;
        }

        if      (cmd=="q")   break;
        else if (cmd=="g")   amp->adjustGain(val);
        else if (cmd=="dr")  amp->adjustDrive(val);
        else if (cmd=="d")   amp->adjustDistortionLevel(val);
        else if (cmd=="s")   amp->adjustEffectSmoothing(val);
        else if (cmd=="dt")  amp->adjustDelayTime(val);
        else if (cmd=="df")  amp->adjustDelayFadeFactor(val);
        else if (cmd=="ds")  amp->flipDistortion();
        else if (cmd=="dl")  amp->flipDelay();
        else if (cmd=="l")   amp->flipLoop();
        else if (cmd=="r")   amp->toggleLoopRecord();
        else if (cmd=="o")   amp->toggleLoopOverdub();
        else if (cmd=="lr")  amp->resetLoop();
        else if (cmd=="del") amp->resetDelay();
        else cout << "? unknown command\n";
    }
}

int main() {

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);

    config.capture.format   = ma_format_f32;   // float samples in
    config.capture.channels = 1;               // guitar = mono
    config.playback.format  = ma_format_f32;   // float samples out
    config.playback.channels = 2;              // stereo output
    config.sampleRate       = 48000;
    config.periodSizeInFrames = 128;           // requested buffer size
    config.dataCallback     = audio_process;   // <-- handing it over 

    Amp amp;
    config.pUserData = &amp;    //pass amp to the callback

    ma_device device;
    if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
        cout << "Failed to initialize audio device\n";
        return -1;
    }
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