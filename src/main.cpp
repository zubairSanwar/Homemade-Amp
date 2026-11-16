#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <limits>
#define MINIAUDIO_IMPLEMENTATION
#include "../external/miniaudio.h"

#include "amp/amp.h"

using namespace std;

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
         << "  wah      -> Auto-wah: w, Sensitivity: ws <val> (0.5..30), Resonance: wq <val> (1..8)\n"
         << "  tremolo  -> Tremolo: trem, Rate: tr <val>, Depth: tres <val>\n"
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
                      ||cmd=="eb"||cmd=="em"||cmd=="et"||cmd=="gt"||cmd=="m"||cmd=="revm"||cmd=="rrs"||cmd=="rvd"||cmd=="ws"||cmd=="wq"
                      ||cmd=="tr"||cmd=="tres");
        if (needsVal && !(cin >> val)) {
            //a failed read latches the stream into a failed state and every later read
            //returns false, so without this one typo would silently end the session
            cin.clear();                                           //drop the error flag
            cin.ignore(numeric_limits<streamsize>::max(), '\n');   //bin the rest of the line
            cout << "  need a number, e.g. " << cmd << " 1.5\n";
            continue;
        }

        if      (cmd=="q")   break;
        else if (cmd=="tr") amp->adjustTremoloRate(val);
        else if (cmd=="tres") amp->adjustTremoloResonance(val);
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
        else if (cmd=="w")   amp->flipWah();
        else if (cmd=="trem") amp->flipTremolo();
        else if (cmd=="ws")  amp->adjustWahSensitivity(val);
        else if (cmd=="wq")  amp->adjustWahResonance(val);
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
