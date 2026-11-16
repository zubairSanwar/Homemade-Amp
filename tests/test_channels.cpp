/*
CHANNEL-LAYOUT TESTS

These drive the real audio_process() from src/main.cpp against fake ma_devices with
different channel layouts, so the "works on any interface" claim is checked rather
than assumed. No audio device is opened - we only fill in the few ma_device fields
the callback reads (capture.channels, playback.channels, pUserData).

src/main.cpp is compiled with -Dmain=amp_main by run_tests.sh so its main() does not
clash with ours. See tests/run_tests.sh.

Why this file exists: forcing a channel count that CoreAudio did not have produced a
silent stream for two sessions, and the guitar channel used to be hardcoded to 1 for
one specific interface. Both are hardware assumptions that no offline DSP test touches.
*/
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include "../external/miniaudio.h"
#include "../src/amp/amp.h"

extern void audio_process(ma_device*, void*, const void*, ma_uint32);

static int fails = 0;
static void check(bool ok, const char* name){ printf("%s  %s\n", ok?"PASS":"FAIL", name); if(!ok) fails++; }

//only the three fields the callback actually touches
static void fakeDevice(ma_device& d, Amp& a, ma_uint32 inCh, ma_uint32 outCh){
    memset(&d, 0, sizeof(d));
    d.capture.channels  = inCh;
    d.playback.channels = outCh;
    d.pUserData = &a;
}

//exact-value tests need the always-on effects out of the way
static void cleanPath(Amp& a){ a.flipCabinet(); a.flipEQ(); a.flipGate(); }

int main(){
    const ma_uint32 N = 128;        // the buffer size we ask the driver for

    // 1. Stereo in / stereo out with the guitar on channel 1 - the M-Audio layout.
    {
        Amp a; a.init(48000); cleanPath(a); a.inputChannels = 2; a.inputChannel = 1;
        ma_device d; fakeDevice(d, a, 2, 2);
        std::vector<float> in(N*2), out(N*2, -99.0f);
        for (ma_uint32 i=0;i<N;i++){ in[i*2+0] = 0.0f; in[i*2+1] = 0.5f; }
        audio_process(&d, out.data(), in.data(), N);

        bool ok = true;
        for (ma_uint32 i=0;i<N;i++)
            if (fabsf(out[i*2+0]-0.5f)>1e-6f || fabsf(out[i*2+1]-0.5f)>1e-6f) ok = false;
        check(ok, "stereo in: reads the chosen channel, same signal to every output");
        check(fabsf(a.peakCh[1].load()-0.5f)<1e-6f && a.peakCh[0].load()==0.0f,
              "stereo in: per-channel meters show which socket is live");
    }

    // 2. Same interface, guitar in the other socket.
    {
        Amp a; a.init(48000); cleanPath(a); a.inputChannels = 2; a.inputChannel = 0;
        ma_device d; fakeDevice(d, a, 2, 2);
        std::vector<float> in(N*2), out(N*2);
        for (ma_uint32 i=0;i<N;i++){ in[i*2+0] = 0.25f; in[i*2+1] = 0.0f; }
        audio_process(&d, out.data(), in.data(), N);

        bool ok = true;
        for (ma_uint32 i=0;i<N;i++) if (fabsf(out[i*2+0]-0.25f)>1e-6f) ok = false;
        check(ok, "stereo in: channel 0 works just as well as channel 1");
    }

    // 3. Mono in / stereo out - the built-in laptop mic into headphones.
    {
        Amp a; a.init(48000); cleanPath(a); a.inputChannels = 1; a.inputChannel = 0;
        ma_device d; fakeDevice(d, a, 1, 2);
        std::vector<float> in(N, 0.3f), out(N*2);
        audio_process(&d, out.data(), in.data(), N);

        bool ok = true;
        for (ma_uint32 i=0;i<N;i++)
            if (fabsf(out[i*2+0]-0.3f)>1e-6f || fabsf(out[i*2+1]-0.3f)>1e-6f) ok = false;
        check(ok, "mono in / stereo out works");
    }

    // 4. The old hardcoded default (channel 1) on a mono device must clamp, not
    //    read the next frame's sample.
    {
        Amp a; a.init(48000); cleanPath(a); a.inputChannels = 1; a.inputChannel = 1;
        ma_device d; fakeDevice(d, a, 1, 1);
        std::vector<float> in(N, 0.4f), out(N);
        audio_process(&d, out.data(), in.data(), N);

        bool ok = true;
        for (ma_uint32 i=0;i<N;i++) if (fabsf(out[i]-0.4f)>1e-6f) ok = false;
        check(ok, "out-of-range channel clamps instead of reading past the frame");
    }

    // 5. A nonsense channel index from a typo.
    {
        Amp a; a.init(48000); a.inputChannels = 2; a.inputChannel = 9999;
        ma_device d; fakeDevice(d, a, 2, 2);
        std::vector<float> in(N*2, 0.2f), out(N*2);
        audio_process(&d, out.data(), in.data(), N);

        bool fin = true;
        for (ma_uint32 i=0;i<N*2;i++) if (!std::isfinite(out[i])) fin = false;
        check(fin, "absurd channel index stays in bounds and finite");
    }

    // 6. An 8-in interface with the guitar a long way down the list.
    {
        Amp a; a.init(48000); cleanPath(a); a.inputChannels = 8; a.inputChannel = 5;
        ma_device d; fakeDevice(d, a, 8, 1);
        std::vector<float> in(N*8, 0.0f), out(N);
        for (ma_uint32 i=0;i<N;i++) in[i*8+5] = 0.6f;
        audio_process(&d, out.data(), in.data(), N);

        bool ok = true;
        for (ma_uint32 i=0;i<N;i++) if (fabsf(out[i]-0.6f)>1e-6f) ok = false;
        check(ok, "8-channel interface: picks channel 5 correctly");
        check(fabsf(a.peakCh[5].load()-0.6f)<1e-6f, "8-channel interface: meter 5 is the hot one");
    }

    // 7. More channels than we meter. Audio must still be right; only the meters cap.
    {
        Amp a; a.init(48000); cleanPath(a); a.inputChannels = 8; a.inputChannel = 3;
        ma_device d; fakeDevice(d, a, 16, 2);
        std::vector<float> in(N*16, 0.0f), out(N*2);
        for (ma_uint32 i=0;i<N;i++) in[i*16+3] = 0.7f;
        audio_process(&d, out.data(), in.data(), N);

        bool ok = true;
        for (ma_uint32 i=0;i<N;i++) if (fabsf(out[i*2]-0.7f)>1e-6f) ok = false;
        check(ok, "16-channel interface still works (meters cap at 8, audio does not)");
    }

    // 8. The underflow guard. Without it `ch = inCh - 1` wraps to 4294967295 and the
    //    next line reads miles out of bounds - verified: removing the guard SEGVs here.
    {
        Amp a; a.init(48000); a.inputChannels = 1;
        ma_device d; fakeDevice(d, a, 0, 2);
        std::vector<float> out(N*2, 7.0f);
        audio_process(&d, out.data(), NULL, N);

        bool silent = true;
        for (ma_uint32 i=0;i<N*2;i++) if (out[i] != 0.0f) silent = false;
        check(silent, "zero capture channels / NULL input emits silence, no out-of-bounds read");
    }

    printf(fails ? "\n%d channel test(s) FAILED\n" : "\nall channel tests passed\n", fails);
    return fails ? 1 : 0;
}
