/*
AMP IMPLEMENTATION WHICH CONNECTS ALL EFFECTS INTO A SINGLE AMP
*/

#include "amp.h"
#include <cmath>

void Amp::init(int sRate){
    sampleRate = sRate;

    loop.init(sampleRate);
    delay.init(sampleRate);
    cabinet.init(sampleRate);
    eq.init(sampleRate);
    gate.init(sampleRate);
    reverb.init(sampleRate);
    wah.init(sampleRate);
    tremolo.init(sampleRate);
}

//adjusting internal values of each effect
void Amp::adjustGain(float g){
    if(g>20.0f) g=20.0f;
    gain.new_gain = g;
}
void Amp::adjustDrive(float d){
    if (d<1.0f) d=1.0f;
    if (d>30.0f) d=30.0f;
    distortion.new_drive = d;
}
void Amp::adjustEffectSmoothing(float s){
    if(s<0) s=0.0f;
    if(s>1.0f) s=1.0f;
    gain.smooth = s;
    distortion.smooth = s;
}
void Amp::adjustDistortionLevel(float l){
    if(l<0.0f) l = 0.0f;
    if(l>1.0f) l = 1.0f;
    distortion.out_level = l;
}
void Amp::adjustDelayTime(float t){
    if (t>2.0f) t=2.0f;
    if (t<0.1f) t=0.1f;
    delay.delay_t = t;
}
void Amp::adjustEQLow(float db){
    if (db<-15.0f) db=-15.0f;
    if (db> 15.0f) db= 15.0f;
    eq.lowDb = db; eq.dirty = true;
}
void Amp::adjustEQMid(float db){
    if (db<-15.0f) db=-15.0f;
    if (db> 15.0f) db= 15.0f;
    eq.midDb = db; eq.dirty = true;
}
void Amp::adjustEQHigh(float db){
    if (db<-15.0f) db=-15.0f;
    if (db> 15.0f) db= 15.0f;
    eq.highDb = db; eq.dirty = true;
}
void Amp::adjustGateThreshold(float db){
    if (db<-90.0f) db=-90.0f;
    if (db>  0.0f) db=  0.0f;
    gate.threshold = powf(10.0f, db/20.0f);
}
void Amp::adjustDelayFadeFactor(float f){
    if (f>0.95f) f = 0.95f;
    if (f<0.0f) f = 0.0f;
    delay.fade_factor = f;
}
void Amp::adjustMaster(float v){
    if(v>1.0f) v=1.0f;
    if(v<0.0f) v=0.0f;
    master = v;
}
void Amp::adjustRoomSize(float rm){
    if(rm>0.9f) rm=0.9f;
    if(rm<0.0f) rm=0.0f;
    reverb.roomSize = rm;
}
void Amp::adjustReverbMix(float m){
    if(m>1.0f) m = 1.0f;
    if(m<0.0f) m = 0.0f;
    reverb.mix = m;
}
void Amp::adjustReverbDamp(float d){   //0 = bright tail, 1 = dark tail
    if(d>0.95f) d = 0.95f;
    if(d<0.0f)  d = 0.0f;
    reverb.damp = d;
}

void Amp::adjustWahSensitivity(float s){
    if(s<0.5f) s = 0.5f;
    if(s>30.0f) s = 30.0f;
    wah.sensitivity = s;
}
void Amp::adjustWahResonance(float q){
    if(q<1.0f) q = 1.0f;
    if(q>8.0f) q = 8.0f;
    wah.resonance = q;
}

void Amp::adjustTremoloRate(float r){
    if (r>20.0f) r=20.0f;
    if (r<0.1f) r=0.1f;
    tremolo.rate = r;
}
void Amp::adjustTremoloResonance(float r){
    if (r>1.0f) r=1.0f;
    if (r<0.0f) r=0.0f;
    tremolo.resonance = r;
}

void Amp::toggleLoopRecord(){
    loop.recordReq = true;
}
void Amp::toggleLoopOverdub(){
    loop.overdubReq = true;
}

//setting effects on / off
void Amp::flipDistortion(){
    distortion_on=!distortion_on;
}
void Amp::flipLoop(){
    loop_on=!loop_on;
}
void Amp::flipDelay(){
    delay_on=!delay_on;
}
void Amp::flipCabinet(){
    cabinet_on=!cabinet_on;
}
void Amp::flipEQ(){
    eq_on=!eq_on;
}
void Amp::flipGate(){
    gate_on=!gate_on;
}
void Amp::flipReverb(){
    reverb_on=!reverb_on;
}
void Amp::flipWah(){
    wah_on=!wah_on;
}
void Amp::flipTremolo(){
    tremolo_on=!tremolo_on;
}

//resetting memory effects
void Amp::resetLoop(){
    loop.clearReq = true;
}
void Amp::resetDelay(){
    delay.clearReq = true;
}

//audio thread: pick up anything the main thread asked for, once per buffer
void Amp::applyControls(){
    loop.applyControls();
    delay.applyControls();
    eq.applyControls();     //rebuild filter coefficients if a tone knob moved
}

//applying effects depending on whether active or not
float Amp::processAudio(float sample){
    float op = sample;

    if (gate_on) op = gate.applyGate(op);   //gate first to remove hissing -> clean sound
    if (loop_on) op = loop.applyLoop(op);   //loop is just clean guitar sound so comes first
    op = gain.applyGain(op);
    if (wah_on) op = wah.applyWah(op);   //wah feeds the distortion, like a pedal in front of an amp
    if (distortion_on) op=distortion.applyDistortion(op);
    if (eq_on) op = eq.applyEQ(op); //tone adjusted post distortion
    if (tremolo_on) op = tremolo.applyTremolo(op);
    if (cabinet_on) op = cabinet.applyCabinet(op);   //the speaker effect
    if (delay_on) op=delay.applyDelay(op);
    if (reverb_on) op=reverb.applyReverb(op);   //at end so we echo final sound

    return op*master;   //apply master volume
}
