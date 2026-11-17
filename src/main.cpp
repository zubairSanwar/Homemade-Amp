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
#include <iomanip>
#define MINIAUDIO_IMPLEMENTATION
#include "../external/miniaudio.h"
#include "../external/httplib.h"

#include "amp/amp.h"

using namespace std;

static httplib::Server server;
static std::thread serverThread;

//Raised by /quit in GUI mode to exit cleanly
static std::atomic<bool> quitRequested{false};

//---- SERVER ENDPOINTS ---

void startServer(Amp* amp){
    if (!server.set_mount_point("/", "./ui")) {
        //mount and check if /ui exists
        cout << "  note: ./ui not found, serving the API only\n";
    }

    //for adjusting knob requests -> takes target float as req
    server.Get("/knob", [amp](const httplib::Request& req, httplib::Response& res) {

        if (!req.has_param("name") || !req.has_param("value")) {
            res.status = 400;
            res.set_content("need both name and value, e.g. /knob?name=adj_gain&value=2.5\n", "text/plain");
            return;
        }

        string name = req.get_param_value("name");
        float v = (float)atof(req.get_param_value("value").c_str());

        //every setter manages its own range -> no check needed here
        if (name=="adj_gain") amp->adjustGain(v);
        else if (name=="adj_drive") amp->adjustDrive(v);
        else if (name=="adj_smooth") amp->adjustEffectSmoothing(v);
        else if (name=="adj_distortion") amp->adjustDistortionLevel(v);
        else if (name=="adj_delay") amp->adjustDelayTime(v);
        else if (name=="adj_delay_fade") amp->adjustDelayFadeFactor(v);
        else if (name=="adj_eq_low") amp->adjustEQLow(v);
        else if (name=="adj_eq_mid") amp->adjustEQMid(v);
        else if (name=="adj_eq_high") amp->adjustEQHigh(v);
        else if (name=="adj_gate") amp->adjustGateThreshold(v);
        else if (name=="adj_master") amp->adjustMaster(v);
        else if (name=="adj_room_size") amp->adjustRoomSize(v);
        else if (name=="adj_rev_mix") amp->adjustReverbMix(v);
        else if (name=="adj_rev_damp") amp->adjustReverbDamp(v);
        else if (name=="wah_sensitivity") amp->adjustWahSensitivity(v);
        else if (name=="wah_resonance") amp->adjustWahResonance(v);
        else if (name=="wah_mix") amp->adjustWahMix(v);
        else if (name=="tremolo_rate") amp->adjustTremoloRate(v);
        else if (name=="tremolo_depth") amp->adjustTremoloDepth(v);
        else{
            res.status = 400;
            res.set_content("unknown knob '" + name + "'\n", "text/plain");
            return;
        }

        res.set_content("ok", "text/plain");
    });

    //on/off states and one-shot actions
    server.Get("/switch", [amp](const httplib::Request& req, httplib::Response& res){
        if (!req.has_param("name")){
            res.status = 400;
            res.set_content("need a name, e.g. /switch?name=eq&value=1\n", "text/plain");
            return;
        }

        string name = req.get_param_value("name");

        //One-shot actions. These carry no value because they are events, not states:
        //the audio thread owns the looper's state and consumes the request flag.
        if (name=="loop_record")  { amp->toggleLoopRecord();  res.set_content("ok","text/plain"); return; }
        if (name=="loop_overdub") { amp->toggleLoopOverdub(); res.set_content("ok","text/plain"); return; }
        if (name=="loop_reset")   { amp->resetLoop();         res.set_content("ok","text/plain"); return; }
        if (name=="delay_reset")  { amp->resetDelay();        res.set_content("ok","text/plain"); return; }

        //state changes accept target state to set to
        if (!req.has_param("value")){
            res.status = 400;
            res.set_content("switch '" + name + "' needs value=0 or value=1\n", "text/plain");
            return;
        }
        bool on = (req.get_param_value("value") == "1");

        if (name=="distortion") amp->setDistortion(on);
        else if (name=="loop") amp->setLoop(on);
        else if (name=="delay") amp->setDelay(on);
        else if (name=="cabinet") amp->setCabinet(on);
        else if (name=="eq") amp->setEQ(on);
        else if (name=="gate") amp->setGate(on);
        else if (name=="reverb") amp->setReverb(on);
        else if (name=="wah") amp->setWah(on);
        else if (name=="tremolo") amp->setTremolo(on);
        else{
            res.status = 400;
            res.set_content("unknown switch '" + name + "'\n", "text/plain");
            return;
        }

        res.set_content("ok", "text/plain");
    });

    //
    server.Get("/results", [amp](const httplib::Request&, httplib::Response& res) {
        const int n = amp->inputChannels;
        int sel = amp->inputChannel.load();
        if (sel < 0 || sel >= n) sel = 0;

        //Read and reset every channel in one pass
        float ch[Amp::MAX_METER_CH];
        for (int c = 0; c < n; ++c) ch[c] = amp->peakCh[c].exchange(0.0f);

        //[selectedIn, out, ch0, ch1, ...] - the first two keep their old meaning
        string j = "[" + to_string(ch[sel]) + "," + to_string(amp->peakOut.exchange(0.0f));
        for (int c = 0; c < n; ++c) j += "," + to_string(ch[c]);
        j += "]";
        res.set_content(j, "application/json");
    });

    //every current value, so the page can draw the knobs in the right positions on
    //load instead of guessing defaults that may already have been changed
    server.Get("/state", [amp](const httplib::Request&, httplib::Response& res) {
        string j = "{";
        j += "\"adj_gain\":"         + to_string(amp->gain.new_gain.load());
        j += ",\"adj_smooth\":"      + to_string(amp->gain.smooth.load());
        j += ",\"adj_drive\":"       + to_string(amp->distortion.new_drive.load());
        j += ",\"adj_distortion\":"  + to_string(amp->distortion.out_level.load());
        j += ",\"adj_delay\":"       + to_string(amp->delay.delay_t.load());
        j += ",\"adj_delay_fade\":"  + to_string(amp->delay.fade_factor.load());
        j += ",\"adj_eq_low\":"      + to_string(amp->eq.lowDb.load());
        j += ",\"adj_eq_mid\":"      + to_string(amp->eq.midDb.load());
        j += ",\"adj_eq_high\":"     + to_string(amp->eq.highDb.load());
        //stored as a linear amplitude but set in dBFS, so convert back for the page
        j += ",\"adj_gate\":"        + to_string(20.0f * log10f(amp->gate.threshold.load()));
        j += ",\"adj_master\":"      + to_string(amp->master.load());
        j += ",\"adj_room_size\":"   + to_string(amp->reverb.roomSize.load());
        j += ",\"adj_rev_mix\":"     + to_string(amp->reverb.mix.load());
        j += ",\"adj_rev_damp\":"    + to_string(amp->reverb.damp.load());
        j += ",\"wah_sensitivity\":" + to_string(amp->wah.sensitivity.load());
        j += ",\"wah_resonance\":"   + to_string(amp->wah.resonance.load());
        j += ",\"wah_mix\":"         + to_string(amp->wah.mix.load());
        j += ",\"tremolo_rate\":"    + to_string(amp->tremolo.rate.load());
        j += ",\"tremolo_depth\":"   + to_string(amp->tremolo.depth.load());

        j += ",\"distortion\":" + string(amp->distortion_on.load() ? "true" : "false");
        j += ",\"loop\":"       + string(amp->loop_on.load()       ? "true" : "false");
        j += ",\"delay\":"      + string(amp->delay_on.load()      ? "true" : "false");
        j += ",\"cabinet\":"    + string(amp->cabinet_on.load()    ? "true" : "false");
        j += ",\"eq\":"         + string(amp->eq_on.load()         ? "true" : "false");
        j += ",\"gate\":"       + string(amp->gate_on.load()       ? "true" : "false");
        j += ",\"reverb\":"     + string(amp->reverb_on.load()     ? "true" : "false");
        j += ",\"wah\":"        + string(amp->wah_on.load()        ? "true" : "false");
        j += ",\"tremolo\":"    + string(amp->tremolo_on.load()    ? "true" : "false");

        j += ",\"sample_rate\":"    + to_string(amp->sampleRate);
        j += ",\"input_channel\":"  + to_string(amp->inputChannel.load());
        j += ",\"input_channels\":" + to_string(amp->inputChannels);
        j += "}";
        res.set_content(j, "application/json");
    });

    //Which input socket the guitar is in. The page sets this from its channel meters
    server.Get("/input", [amp](const httplib::Request& req, httplib::Response& res){
        if (!req.has_param("channel")){
            res.status = 400;
            res.set_content("need a channel, e.g. /input?channel=1\n", "text/plain");
            return;
        }
        int n = atoi(req.get_param_value("channel").c_str());
        if (n < 0) n = 0;
        if (n > amp->inputChannels - 1) n = amp->inputChannels - 1;
        amp->inputChannel = n;
        res.set_content("ok", "text/plain");
    });

    //for clean exit
    server.Get("/quit", [](const httplib::Request&, httplib::Response& res) {
        quitRequested = true;
        res.set_content("shutting down\n", "text/plain");
    });

    //Anything that reached no handler -> send specific error msg
    server.set_error_handler([](const httplib::Request& req, httplib::Response& res){
        if (res.body.empty())
            res.set_content("no handler for " + req.path + "\n", "text/plain");
    });

    serverThread = std::thread([] {
        //listen() blocks until stop(). It returns false if the port is already in use
        if (!server.listen("127.0.0.1", 8080))
            cout << "\n  WARNING: could not bind 127.0.0.1:8080 (already in use?)"
                    " - the GUI will not be able to connect\n";
    });
}

void stopServer() {
    if (!serverThread.joinable()) return;   //never started, nothing to wind down
    server.stop();
    serverThread.join();
}

//--- AUDIO PROCESSING ---

void audio_process(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {

    Amp* amp = (Amp*)pDevice->pUserData;

    float* out = (float*)pOutput;
    const float* in = (const float*)pInput;

    //The device picks its own channel counts, so read them rather than assume.
    const ma_uint32 inCh  = pDevice->capture.channels;
    const ma_uint32 outCh = pDevice->playback.channels;

    //Nothing sane to read from. Emit silence instead of indexing into a buffer that is not there
    if (in == NULL || inCh == 0) {
        for (ma_uint32 i = 0; i < frameCount * outCh; ++i) out[i] = 0.0f;
        return;
    }

    amp->applyControls();   //apply any knob presses once here, not per sample

    //pick the guitar's channel once per buffer
    ma_uint32 ch = (ma_uint32)amp->inputChannel.load();
    if (ch >= inCh) ch = inCh - 1;      //safe: inCh is at least 1 by here

    //Peak per input channel, so `pk` can show which socket the guitar is really in.
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

//--- AUTO SELECT GUITAR CHANNEL ---

//How long the auto-detect listens for. Long enough for user to actually play guitar. Adjust as needed
static const float DETECT_SECONDS = 6.0f;

int detectInputChannel(Amp* amp, float seconds) {
    const float FLOOR = 0.01f;   //~-40 dBFS: above any interface's idle hiss, below a real note

    for (int c = 0; c < Amp::MAX_METER_CH; ++c) amp->peakCh[c].store(0.0f);

    //Count down out loud with the live per-channel peaks, so it is obvious the amp is listening
    for (int left = (int)seconds; left > 0; --left) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cout << "\r  " << (left - 1) << "s ";
        for (int c = 0; c < amp->inputChannels; ++c)
            cout << "   ch" << c << ": " << fixed << setprecision(3) << amp->peakCh[c].load();
        cout << "    " << flush;
    }
    cout << "\n";

    int best = -1;
    float bestPeak = FLOOR;
    for (int c = 0; c < amp->inputChannels; ++c) {
        float p = amp->peakCh[c].load();
        if (p > bestPeak) { bestPeak = p; best = c; }
    }
    return best;
}

//--- CMD LINE AMP CONTROLS ---

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
        if (!(cin >> cmd)) {
            cout << "\nno more input on stdin - exiting."
                 << " Run from a terminal, or use `./amp gui`.\n";
            break;
        }

        //commands that expect a number after them
        bool needsVal = (cmd=="g"||cmd=="dr"||cmd=="d"||cmd=="s"||cmd=="dt"||cmd=="df"
                      ||cmd=="eb"||cmd=="em"||cmd=="et"||cmd=="gt"||cmd=="m"||cmd=="revm"||cmd=="rrs"||cmd=="rvd"||cmd=="ws"||cmd=="wq"||cmd=="wm"
                      ||cmd=="tr"||cmd=="trd");
        if (needsVal && !(cin >> val)) {
            //a failed read latches the stream into a failed state and every later read returns false
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
                cout << "  listening " << (int)DETECT_SECONDS << " s - play now...\n" << flush;
                int found = detectInputChannel(amp, DETECT_SECONDS);
                if (found >= 0) {
                    amp->inputChannel = found;
                    cout << "  guitar on input channel " << found << "\n";
                } else {
                    cout << "  nothing above the noise floor - left on channel "
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

int main(int argc, char** argv) {

    //Which control surface to run. Either: cmd line or GUI
    bool useGui = false;
    if (argc > 1) {
        string mode = argv[1];
        if      (mode=="gui" || mode=="--gui") useGui = true;
        else if (mode=="cli" || mode=="--cli") useGui = false;
        else {
            cout << "usage: " << argv[0] << " [cli|gui]\n"
                 << "  cli   type commands in this terminal (default)\n"
                 << "  gui   serve the web UI on http://127.0.0.1:8080\n";
            
            return (mode=="-h" || mode=="--help") ? 0 : 1;
        }
    }


    ma_device_config config = ma_device_config_init(ma_device_type_duplex);

    //Setting up miniaudio
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

    //Just sanity check of what driver handed us
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
    if (useGui) {
        cout << "\nInput channel: starting on 0 - pick the right one in the browser.\n";
    } else if (amp.inputChannels > 1) {
        cout << "\nFinding the guitar - play a few notes...\n" << flush;
        int found = detectInputChannel(&amp, DETECT_SECONDS);
        if (found >= 0) {
            amp.inputChannel = found;
            cout << "  guitar on input channel " << found << "\n";
        } else {
            cout << "  nothing heard - starting on channel 0."
                 << " Use `pk` to see every channel, or `ic auto` to retry.\n";
        }
    } else {
        //Say so rather than silently skipping
        cout << "\nOnly one input channel, so nothing to detect - using channel 0.\n";
    }

    if (useGui) {   //setting up server GUI model
        startServer(&amp);
        cout << "\nControl: web UI on http://127.0.0.1:8080\n";
        cout << "Audio running. Ctrl-C to stop, or open /quit.\n\n";

        //Nothing to read from in GUI mode, so just idle until the page says quit.
        while (!quitRequested.load()){
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        cout << "shutting down\n";
        stopServer();
    } else {    //getting controls from terminal
        cout << "\nControl: this terminal. Run with `gui` for the web UI.\n";
        cout << "\nAudio running.\n\n";
        handle_inputs(&amp);    // keeps main alive; the audio runs on another thread
    }

    ma_device_uninit(&device);
    return 0;
}
