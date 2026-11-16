#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <cstdlib>
#include <thread>
#include <chrono>
#define MINIAUDIO_IMPLEMENTATION
#include "../external/miniaudio.h"

#include "amp/amp.h"

using namespace std;

void audio_process(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {

    Amp* amp = (Amp*)pDevice->pUserData;

    float* out = (float*)pOutput;
    const float* in = (const float*)pInput;

    //The device picks its own channel counts, so read them rather than assume.
    //Both are fixed at init time, so they are safe to read from the audio thread.
    const ma_uint32 inCh  = pDevice->capture.channels;
    const ma_uint32 outCh = pDevice->playback.channels;

    //Nothing sane to read from. Emit silence instead of indexing into a buffer that
    //is not there - and note that the clamp below would underflow if inCh were 0.
    if (in == NULL || inCh == 0) {
        for (ma_uint32 i = 0; i < frameCount * outCh; ++i) out[i] = 0.0f;
        return;
    }

    amp->applyControls();   //apply any knob presses once here, not per sample

    //pick the guitar's channel once per buffer, clamped so we can never read past
    //the end of a frame no matter what the user sets
    ma_uint32 ch = (ma_uint32)amp->inputChannel.load();
    if (ch >= inCh) ch = inCh - 1;      //safe: inCh is at least 1 by here

    //Peak per input channel, so `pk` can show which socket the guitar is really in.
    //Kept in a stack array and published once per buffer, not once per sample.
    const ma_uint32 meterCh = (inCh < (ma_uint32)Amp::MAX_METER_CH) ? inCh : (ma_uint32)Amp::MAX_METER_CH;
    float chPeak[Amp::MAX_METER_CH] = {0.0f};
    float pout = 0.0f;

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        //interleaved: frame i starts at i*inCh, then step to the channel we want
        const float* frame = in + (size_t)i * inCh;

        for (ma_uint32 c = 0; c < meterCh; ++c) {
            float a = fabsf(frame[c]);
            if (a > chPeak[c]) chPeak[c] = a;
        }

        float op = amp->processAudio(frame[ch]);

        //clipping input between fixed range to prevent issues in effects (e.g. in delay we might record faulty value leading to noise until reset)
        if (!isfinite(op)) op = 0.0f;
        if (op >  1.0f) op =  1.0f;
        if (op < -1.0f) op = -1.0f;

        if (fabsf(op) > pout) pout = fabsf(op);

        //mono out to however many output channels the device has (same signal to each)
        for (ma_uint32 c = 0; c < outCh; ++c) out[(size_t)i * outCh + c] = op;
    }

    for (ma_uint32 c = 0; c < meterCh; ++c)
        if (chPeak[c] > amp->peakCh[c].load()) amp->peakCh[c].store(chPeak[c]);
    if (pout > amp->peakOut.load()) amp->peakOut.store(pout);
}

//Listen for a moment and return the input channel carrying the strongest signal,
//or -1 if nothing rose above the noise floor. This runs on the MAIN thread and only
//reads the meters the callback publishes, so the audio thread is left alone - the
//"no sleeps" rule is about the callback, not about here.
int detectInputChannel(Amp* amp, float seconds) {
    const float FLOOR = 0.01f;   //~-40 dBFS: above any interface's idle hiss, below a real note

    for (int c = 0; c < Amp::MAX_METER_CH; ++c) amp->peakCh[c].store(0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(seconds * 1000.0f)));

    int best = -1;
    float bestPeak = FLOOR;
    for (int c = 0; c < amp->inputChannels; ++c) {
        float p = amp->peakCh[c].load();
        if (p > bestPeak) { bestPeak = p; best = c; }
    }
    return best;
}

void handle_inputs(Amp* amp){   //allow control knobs through cmd line -> for testing
    cout << "Adjust: <cmd> <value>    Toggle/reset: <cmd>\n"
         << "  values   -> Gain: g, Drive: dr, Dist level: d, Smooth: s, Delay time: dt, Delay fade: df, Master: m\n"
         << "  switches -> Distortion: ds, Delay: dl, Loop: l, Cabinet: cb, EQ: eq, Gate: gs\n"
         << "  tone     -> Bass: eb <dB>, Mid: em <dB>, Treble: et <dB>  (-15..+15)\n"
         << "  gate     -> Threshold: gt <dBFS>  (try -50)\n"
         << "  reverb   -> Reverb: rev, Reverb Mix: revm <val>, Room Size: rrs <val>, Damping: rvd <val>\n"
         << "  wah      -> Auto-wah: w, Sensitivity: ws <val> (0.5..30), Resonance: wq <val> (1..8), Mix: wm <val> (0..1)\n"
         << "  tremolo  -> Tremolo: trem, Rate: tr <val>, Depth: trd <val> (0..1)\n"
         << "  looper   -> Record: r, Overdub: o, Reset loop: lr, Reset delay: del\n"
         << "  diag     -> Peak levels: pk, Input channel: ic <n> | ic auto\n"
         << "  quit     -> q\n\n";

    string cmd;
    float val;
    while (true) {
        cout << "CMD: " << flush;
        if (!(cin >> cmd)) break;   //end of input / ctrl-D

        //commands that expect a number after them
        bool needsVal = (cmd=="g"||cmd=="dr"||cmd=="d"||cmd=="s"||cmd=="dt"||cmd=="df"
                      ||cmd=="eb"||cmd=="em"||cmd=="et"||cmd=="gt"||cmd=="m"||cmd=="revm"||cmd=="rrs"||cmd=="rvd"||cmd=="ws"||cmd=="wq"||cmd=="wm"
                      ||cmd=="tr"||cmd=="trd");
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
        else if (cmd=="trd") amp->adjustTremoloDepth(val);
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
        else if (cmd=="wm")  amp->adjustWahMix(val);
        else if (cmd=="eb")  amp->adjustEQLow(val);
        else if (cmd=="em")  amp->adjustEQMid(val);
        else if (cmd=="et")  amp->adjustEQHigh(val);
        else if (cmd=="gt")  amp->adjustGateThreshold(val);
        else if (cmd=="r")   amp->toggleLoopRecord();
        else if (cmd=="o")   amp->toggleLoopOverdub();
        else if (cmd=="lr")  amp->resetLoop();
        else if (cmd=="del") amp->resetDelay();
        else if (cmd=="pk") {   //peak since last check - is audio flowing, and on which channel?
            int sel = amp->inputChannel.load();
            cout << "  in ";
            for (int c = 0; c < amp->inputChannels; ++c)
                cout << "ch" << c << ": " << amp->peakCh[c].exchange(0.0f)
                     << (c == sel ? " *   " : "     ");
            cout << "\n  out: " << amp->peakOut.exchange(0.0f) << "\n";
        }
        else if (cmd=="ic") {   //which input channel the guitar is on (0-based), or "auto"
            string arg;
            if (!(cin >> arg)) break;
            if (arg == "auto") {
                cout << "  listening 2 s - play now..." << flush;
                int found = detectInputChannel(amp, 2.0f);
                if (found >= 0) {
                    amp->inputChannel = found;
                    cout << "\n  guitar on input channel " << found << "\n";
                } else {
                    cout << "\n  nothing above the noise floor - left on channel "
                         << amp->inputChannel.load() << "\n";
                }
            } else {
                char* end = NULL;
                long n = strtol(arg.c_str(), &end, 10);
                if (end == arg.c_str() || *end != '\0') {
                    cout << "  need a channel number or the word auto, e.g. ic 1\n";
                } else {
                    if (n < 0) n = 0;
                    if (n > amp->inputChannels - 1) n = amp->inputChannels - 1;
                    amp->inputChannel = (int)n;
                    cout << "  input channel " << n << "\n";
                }
            }
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

    //What the driver actually gave us. Worth printing: channel counts and rate are
    //negotiated, not forced, so they differ from one interface to the next.
    cout << "Capture : " << device.capture.name
         << "  " << device.capture.channels << " ch\n";
    cout << "Playback: " << device.playback.name
         << "  " << device.playback.channels << " ch\n";
    cout << "Sample rate: " << device.sampleRate << "\n";

    amp.inputChannels = (int)device.capture.channels;
    if (amp.inputChannels < 1) amp.inputChannels = 1;
    if (amp.inputChannels > Amp::MAX_METER_CH) amp.inputChannels = Amp::MAX_METER_CH;

    amp.init(device.sampleRate);

    // from here, miniaudio calls audio_process repeatedly
    if (ma_device_start(&device) != MA_SUCCESS) {
        cout << "Failed to start audio device\n";
        ma_device_uninit(&device);
        return -1;
    }

    //Find the guitar instead of assuming it is on the channel my own interface uses.
    //Needs the device running, so this has to come after ma_device_start.
    if (amp.inputChannels > 1) {
        cout << "\nFinding the guitar - play a few notes..." << flush;
        int found = detectInputChannel(&amp, 2.0f);
        if (found >= 0) {
            amp.inputChannel = found;
            cout << "\n  guitar on input channel " << found << "\n";
        } else {
            cout << "\n  nothing heard - starting on channel 0."
                 << " Use `pk` to see every channel, or `ic auto` to retry.\n";
        }
    }

    cout << "\nAudio running.\n\n";

    handle_inputs(&amp);        // keeps main alive; the audio runs on another thread

    ma_device_uninit(&device);
    return 0;
}
