/*
HTTP CONTROL API TESTS

Calls the real startServer() from src/main.cpp and talks to it with httplib's own
client. No audio device is opened and no browser is involved.

src/main.cpp is compiled with -Dmain=amp_main by run_tests.sh so its main() does not
clash with ours. See tests/run_tests.sh.

What these are guarding:
- an unknown or incomplete request must come back as a 4xx WITH a readable message,
  not an empty body
- it must not change the amp, and must not stop the server
- switches take an explicit value, so sending the same thing twice is a no-op

Note: values themselves are NOT validated. The only client is our own local page, so
the request shapes are fixed and atof is enough. Setters clamp their own ranges.
*/
#include <cstdio>
#include <cmath>
#include <string>
#include <thread>
#include <chrono>
#include "../external/httplib.h"
#include "../src/amp/amp.h"

extern void startServer(Amp*);
extern void stopServer();

static int fails = 0;
static void check(bool ok, const char* name){ printf("%s  %s\n", ok?"PASS":"FAIL", name); if(!ok) fails++; }

int main(){
    Amp amp;
    amp.init(48000);
    amp.inputChannels = 2;
    amp.inputChannel  = 1;

    startServer(&amp);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    httplib::Client cli("127.0.0.1", 8080);
    cli.set_connection_timeout(2, 0);

    //a rejected request must say WHY, not just fail silently
    auto rejects = [&](const char* path, const char* name){
        auto r = cli.Get(path);
        if (!r) { printf("FAIL  %s (no response - is port 8080 already in use?)\n", name); fails++; return; }
        bool ok = (r->status == 400) && !r->body.empty();
        printf("%s  %s\n", ok?"PASS":"FAIL", name);
        if (!ok) { printf("        got status %d body '%s'\n", r->status, r->body.c_str()); fails++; }
    };
    auto accepts = [&](const char* path, const char* name){
        auto r = cli.Get(path);
        bool ok = r && r->status == 200;
        printf("%s  %s\n", ok?"PASS":"FAIL", name);
        if (!ok) fails++;
    };

    // ---- input validation ----
    rejects("/knob?name=adj_gain",           "knob with no value is rejected with a message");
    rejects("/knob?name=nonsense&value=1",   "unknown knob name is rejected with a message");
    rejects("/knob",                         "knob with no name at all is rejected");
    rejects("/switch?name=eq",               "switch with no value is rejected");
    rejects("/switch?name=nonsense&value=1", "unknown switch name is rejected");
    rejects("/switch",                       "switch with no name at all is rejected");

    // ---- a rejected request must not change anything ----
    {
        amp.adjustGain(3.0f);
        cli.Get("/knob?name=nonsense&value=9");
        check(fabsf(amp.gain.new_gain.load() - 3.0f) < 1e-6f,
              "a rejected request leaves the knobs where they were");
    }

    // ---- valid requests ----
    accepts("/knob?name=adj_gain&value=2.5", "valid knob request returns 200");
    check(fabsf(amp.gain.new_gain.load() - 2.5f) < 1e-6f, "valid knob request actually moves the knob");

    accepts("/knob?name=adj_gain&value=999", "out-of-range value is accepted");
    check(fabsf(amp.gain.new_gain.load() - 20.0f) < 1e-6f, "out-of-range value is clamped by the setter");

    // ---- switches carry state, not a flip ----
    {
        amp.setEQ(true);
        cli.Get("/switch?name=eq&value=0");
        bool offOnce = !amp.eq_on.load();
        cli.Get("/switch?name=eq&value=0");
        bool offTwice = !amp.eq_on.load();
        check(offOnce && offTwice, "sending the same switch state twice is a no-op (a toggle would flip back)");
    }

    // ---- one-shot actions still work without a value ----
    {
        amp.loop.recordReq = false;
        accepts("/switch?name=loop_record", "one-shot loop record needs no value");
        check(amp.loop.recordReq.load(), "one-shot loop record raises the request flag");
    }

    // ---- the server keeps listening after all of that ----
    accepts("/knob?name=adj_master&value=0.8", "server still serving after every bad request above");
    check(fabsf(amp.master.load() - 0.8f) < 1e-6f, "and still applying values correctly");

    // ---- read-back endpoints ----
    {
        auto r = cli.Get("/state");
        bool ok = r && r->status==200
                  && r->body.find("\"adj_gain\"")  != std::string::npos
                  && r->body.find("\"tremolo\"")   != std::string::npos
                  && r->body.find("\"adj_gate\"")  != std::string::npos;
        check(ok, "/state returns every knob and switch");
    }
    {
        amp.peakCh[1].store(0.42f); amp.peakOut.store(0.31f);
        auto a = cli.Get("/results");
        auto b = cli.Get("/results");
        //a bare [in, out] array - the page destructures it
        bool ok = a && b && a->status==200
                  && a->body == "[0.420000,0.310000]"
                  && b->body == "[0.000000,0.000000]";
        check(ok, "/results is [in,out] and resets the peaks for the next poll");
        if (!ok && a && b) printf("        got '%s' then '%s'\n", a->body.c_str(), b->body.c_str());
    }

    // ---- /quit: the only way out of GUI mode, since there is no terminal there ----
    {
        auto r = cli.Get("/quit");
        check(r && r->status==200 && !r->body.empty(), "/quit answers before shutting down");
    }

    // ---- unknown route gets a body too ----
    {
        auto r = cli.Get("/bogus");
        check(r && r->status==404 && !r->body.empty(), "unknown route returns 404 with a message");
    }

    stopServer();
    printf(fails ? "\n%d API test(s) FAILED\n" : "\nall API tests passed\n", fails);
    return fails ? 1 : 0;
}
