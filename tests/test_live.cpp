// Live settings: live.conf parsing, clamping, bad lines, removal, FRAMEGEN_ keys, and the verbose live.log.
#include "live_config.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void Write(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc); f << text; }
    std::rename(tmp.c_str(), path.c_str());
}
static std::string Slurp(const std::string& path) { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); return s.str(); }
static uint64_t g_now = 1'000'000'000ull;
static void Poll() { g_now += 1'100'000'000ull; LivePoll(g_now); }
static void Touch(const std::string& path, int sec_ahead) {      // the mtime has to differ even inside one test second
    struct timespec ts[2]; clock_gettime(CLOCK_REALTIME, &ts[0]); ts[0].tv_sec += sec_ahead; ts[1] = ts[0];
    utimensat(AT_FDCWD, path.c_str(), ts, 0);
}

int main() {
    {   // TARGET_FPS is limited to a fixed refresh rate, not to a variable one or an unknown one
        const uint64_t hz90 = 11'111'111ull, hz60 = 16'666'667ull;
        CHECK(std::abs(EffectiveTargetFps(120, hz90, false) - 90.0) < 0.01);                 // above the refresh rate: limited
        CHECK(EffectiveTargetFps(90, hz90, false) == 90.0);                                  // equal (inside the slack): untouched
        CHECK(EffectiveTargetFps(60, hz90, false) == 60.0);                                  // below: untouched
        CHECK(EffectiveTargetFps(1000, hz90, true) == 1000.0);                               // variable refresh: not limited
        CHECK(EffectiveTargetFps(120, 0, false) == 120.0);                                   // unknown: not limited
        CHECK(std::abs(EffectiveTargetFps(90, hz60, false) - 60.0) < 0.01);                  // a 60 Hz screen
        CHECK(std::abs(LayerCostRatio(180.0, 45.0, 1.0) - 0.225) < 1e-12);                   // 4 ms x 45 + 1 ms x 45 of a second
        CHECK(LayerCostRatio(900.0, 300.0, 1.0) > 1.0 && LayerCostRatio(1.0, 1.0, 0.0) == 0.0);
    }
    char dir[] = "/tmp/vkilive_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/live.conf", log = std::string(dir) + "/live.log";
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1);
    setenv("VKIFRAME_LIVE_LOG", log.c_str(), 1);
    setenv("VKIFRAME_TARGET_FPS", "90", 1);
    setenv("VKIFRAME_BASE_FPS_CAP", "45", 1);
    setenv("VKIFRAME_WARP", "1", 1);                              // the oldest name of OPTICAL_FLOW still works

    Write(conf, "REV=0\nFRAMEGEN_EXTRAPOLATE=1\nVKIFRAME_UI_MASK_THRESHOLD=0.03\nTARGET_FPS=75\n");
    CHECK(EnvVar("EXTRAPOLATE") && std::string(EnvVar("EXTRAPOLATE")) == "1");          // startup-only keys come from the file
    CHECK(EnvVar("UI_MASK_THRESHOLD") && std::string(EnvVar("UI_MASK_THRESHOLD")) == "0.03");
    CHECK(!EnvVar("FLOW_LEVEL"));                          // not in the file / not a startup key
    unlink(conf.c_str());

    {   // defaults by machine (cap / target): Deck OLED 45 / 90, Deck LCD 30 / 60, any other machine 60 / 120
        const std::string dmi = std::string(dir) + "/dmi";
        auto is = [&](const char* name, float cap, float target) { Write(dmi, name); MachineDefaults m = MachineDefaultsFor(dmi.c_str()); return m.base_fps_cap == cap && m.target_fps == target && DefaultTargetFps(dmi.c_str()) == target && DefaultBaseFpsCap(dmi.c_str()) == cap; };
        CHECK(is("Galileo\n", 45.0f, 90.0f));
        CHECK(is("Jupiter\n", 30.0f, 60.0f));
        CHECK(is("MS-7E12\n", 60.0f, 120.0f));
        CHECK(is("\n", 60.0f, 120.0f));
        CHECK(is("", 60.0f, 120.0f));
        CHECK(MachineDefaultsFor("/nonexistent/dmi").target_fps == 120.0f && MachineDefaultsFor("/nonexistent/dmi").base_fps_cap == 60.0f);
        unlink(dmi.c_str());
    }
    LiveSettings& L = Live();
    CHECK(GetConfig().interpolate);
    Poll();                                                       // no file: environment values
    CHECK(L.enabled == 1 && L.target_fps == 90.0f && L.base_fps_cap == 45.0f && L.max_synthetic == 3);

    Write(conf, "# test\nREV=1\nFRAMEGEN_TARGET_FPS=60\nVKIFRAME_MAX_SYNTHETIC=2\nWARP_BIDIR=1\nENABLED=1\n");
    Poll();
    CHECK(L.target_fps == 60.0f && L.max_synthetic == 2 && L.interp_bidir == 1 && L.base_fps_cap == 45.0f);   // missing keys keep the environment value

    Write(conf, "REV=2\nTARGET_FPS=9999\nMAX_SYNTHETIC=9\nMIN_GAIN=abc\nPHASE_OFFSET=-3\nBASE_FPS_CAP=500\nNONSENSE=1\nWARP=1\nINTERPOLATE=1\nOPTICAL_FLOW=1\nWARP_BIDIR=0\nFLOW_LEVEL=1\nno equals sign\nENABLED=0\n"); Touch(conf, 5);
    Poll();
    CHECK(L.target_fps == 600.0f);                                 // clamped to the schema range
    CHECK(L.max_synthetic == kLiveMaxSyntheticNormal);   // without EXPERIMENTAL at game start the cap is 4
    CHECK(L.min_gain == 1.01f);                                   // invalid value: the previous (startup) value stays
    CHECK(L.phase_offset == 0.0f);
    CHECK(L.base_fps_cap == 240.0f);
    CHECK(L.enabled == 0);
    CHECK(L.interp_bidir == 0);                                   // the old key name WARP_BIDIR works too                                     // dropped from the file: back to the environment value

    Write(conf, "REV=3\nENABLED=1\nTARGET_FPS=75\n"); Touch(conf, 10);
    Poll();
    CHECK(L.enabled == 1 && L.target_fps == 75.0f && L.max_synthetic == 3 && L.base_fps_cap == 45.0f);

    Write(conf, "REV=4\nCMD_BUFFERS=99\n"); Touch(conf, 12); Poll(); CHECK(L.cmd_buffers == 32);
    Write(conf, "REV=5\nCMD_BUFFERS=2\n"); Touch(conf, 14); Poll(); CHECK(L.cmd_buffers == 4);
    Write(conf, "REV=6\nCMD_BUFFERS=16\n"); Touch(conf, 16); Poll(); CHECK(L.cmd_buffers == 16);

    CHECK(L.synth_margin == 0.15f);                               // the default, as in the environment
    Write(conf, "REV=7\nSYNTH_MARGIN=0\n"); Touch(conf, 18); Poll(); CHECK(L.synth_margin == 0.0f);
    Write(conf, "REV=8\nSYNTH_MARGIN=0.9\n"); Touch(conf, 20); Poll(); CHECK(L.synth_margin == 0.45f);
    Write(conf, "REV=9\nSYNTH_MARGIN=-1\n"); Touch(conf, 22); Poll(); CHECK(L.synth_margin == 0.0f);

    {   // hostile or broken file contents: nothing crashes, nothing hangs, nothing broken is applied
        int t = 100;
        auto Put = [&](const std::string& text) { Write(conf, text); Touch(conf, t += 2); Poll(); };
        Put("REV=40\nTARGET_FPS=80\nMAX_SYNTHETIC=3\nPHASE_OFFSET=0.3\nCMD_BUFFERS=10\n");
        CHECK(L.target_fps == 80.0f && L.max_synthetic == 3);
        // a NUL byte: that line is dropped whole, the lines after it still count
        Put(std::string("TARGET_FPS=65\0junk\nMAX_SYNTHETIC=2\n", 35));
        CHECK(L.target_fps == 90.0f && L.max_synthetic == 2);                              // 65 was not applied: the file has no valid TARGET_FPS line, so the startup value (90, environment) stands
        // CRLF line ends are fine; a lone CR (an old Mac file) makes one long line whose value is not a number
        Put("TARGET_FPS=70\r\nMAX_SYNTHETIC=1\r\n");
        CHECK(L.target_fps == 70.0f && L.max_synthetic == 1);
        Put("TARGET_FPS=71\rMAX_SYNTHETIC=3\n");
        CHECK(L.target_fps == 90.0f);                                                       // not a number: the line is invalid, the startup value (environment) applies
        // nan, inf, hex floats, hex ints, text after the number: none of them is a number
        Put("TARGET_FPS=nan\nMIN_GAIN=inf\nPHASE_OFFSET=0x1p-1\nSYNTH_MARGIN=-inf\nCMD_BUFFERS=0x10\nBASE_FPS_CAP=1e999\nMAX_SYNTHETIC=2x\n");
        CHECK(L.target_fps == 90.0f && L.min_gain == 1.01f && L.phase_offset == 0.5f && L.cmd_buffers == 8 && L.synth_margin == 0.15f && L.base_fps_cap == 45.0f && L.max_synthetic == 3);
        // empty key, empty value, only blanks, a bare =
        Put("=5\nTARGET_FPS=\n   =   \n=\n\t\nTARGET_FPS = 66 \n");
        CHECK(L.target_fps == 66.0f);                                                       // blanks around key and value are fine; the broken lines did nothing
        // BASE_FPS_CAP below 1 is off, 1 is a cap, and the cap is not applied at 0.5
        Put("BASE_FPS_CAP=0.0001\n"); CHECK(L.base_fps_cap == 0.0f);
        Put("BASE_FPS_CAP=0.99\n");   CHECK(L.base_fps_cap == 0.0f);
        Put("BASE_FPS_CAP=1\n");      CHECK(L.base_fps_cap == 1.0f);
        Put("BASE_FPS_CAP=500\n");    CHECK(L.base_fps_cap == 240.0f);
        Put("BASE_FPS_CAP=120\n");    CHECK(L.base_fps_cap == 120.0f);                  // a cap of 120 is no longer clamped to 90
        Put("BASE_FPS_CAP=240\n");    CHECK(L.base_fps_cap == 240.0f);
        Put("TARGET_FPS=600\n");      CHECK(L.target_fps == 600.0f);
        Put("TARGET_FPS=601\n");      CHECK(L.target_fps == 600.0f);
        // a file over 64 KiB: the complete lines inside the first 64 KiB are used, the line cut by the limit is dropped (not read as TARGET_FPS=8), the rest is ignored
        {
            std::string big = "MAX_SYNTHETIC=2\n";
            const std::string tail = "TARGET_FPS=88\n";
            const size_t limit = 65536, cut_at = limit - 12;              // the tail starts 12 bytes before the limit: "TARGET_FPS=8" is what the limit leaves of it
            while (big.size() + 1 < cut_at) big += (cut_at - big.size() > 80 ? "#" + std::string(78, 'x') + "\n" : std::string("#\n"));
            if (big.size() < cut_at) big += std::string(cut_at - big.size() - 1, '#') + "\n";
            CHECK(big.size() == cut_at);
            big += tail + "ENABLED=0\n" + std::string(4000, '#');
            Put(big);
            CHECK(L.max_synthetic == 2);                                                   // before the limit: used
            CHECK(L.target_fps == 90.0f);                                                  // the cut line is not TARGET_FPS=8 (clamped to 30), not 88: the startup value stands
            CHECK(L.enabled == 1);                                                         // after the limit: ignored
        }
        // the swapchain capacity request at game start follows the live parser: a MAX_SYNTHETIC line it ignores (5x, 0x4, nan) sets nothing, the last valid line counts
        Put("MAX_SYNTHETIC=5x\n");                 CHECK(LiveRequestedMaxSynthetic() == 0);
        Put("MAX_SYNTHETIC=0x4\n");                CHECK(LiveRequestedMaxSynthetic() == 0);
        Put("MAX_SYNTHETIC=nan\n");                CHECK(LiveRequestedMaxSynthetic() == 0);
        Put("MAX_SYNTHETIC=7\n");                  CHECK(LiveRequestedMaxSynthetic() == 7);
        Put("MAX_SYNTHETIC=8\nMAX_SYNTHETIC=5x\n"); CHECK(LiveRequestedMaxSynthetic() == 8);
        Put("MAX_SYNTHETIC=99\n");                 CHECK(LiveRequestedMaxSynthetic() == 10);
        Put("REV=40\n");
        // a FIFO (or any special file) in the place of live.conf must not block the game: it is refused
        unlink(conf.c_str());
        CHECK(mkfifo(conf.c_str(), 0600) == 0);
        alarm(10);                                                                       // a blocked open() would hang here: the alarm ends the test with a failure
        Poll();
        alarm(0);
        CHECK(L.target_fps == 90.0f);
        unlink(conf.c_str());
        Put("REV=41\nTARGET_FPS=75\n"); CHECK(L.target_fps == 75.0f);                    // a normal file works again
        // control characters in the file never reach live.log, and a file of junk makes a handful of notes, not thousands
        std::string junk = "\x1b[31mred\x07=1\nTARGET_FPS=\x01\x02 5\n";
        for (int i = 0; i < 300; ++i) junk += "BOGUS" + std::to_string(i) + "=1\n";
        Put(junk);
        CHECK(L.target_fps == 90.0f);
    }

    {   // bench.req (next to live.conf): the benchmark request token. It is not a setting.
        const std::string req = std::string(dir) + "/bench.req";
        int t = 40;
        auto Req = [&](const char* text) { Write(req, text); Touch(req, t += 2); Poll(); };
        long long tok = 0;
        CHECK(!LiveBenchRequest(&tok) && !LiveBenchCancel());
        const unsigned long long gen0 = LiveGeneration();
        Req("1700000001\n");
        CHECK(LiveBenchRequest(&tok) && tok == 1700000001ll);
        CHECK(LiveGeneration() == gen0);                          // no live value moved: a request can never count as a changed setting
        LiveBenchAck(tok); CHECK(!LiveBenchRequest(&tok));
        Req("1700000001\n"); CHECK(!LiveBenchRequest(&tok));      // the same number again is not a new request
        Req("1700000002\n"); CHECK(LiveBenchRequest(&tok) && tok == 1700000002ll);
        LiveBenchAck(tok);
        Req("0\n"); CHECK(LiveBenchCancel() && !LiveBenchCancel() && !LiveBenchRequest(&tok));      // 0: stop a running benchmark, once
        Req("0\n"); CHECK(!LiveBenchCancel());
        Req("abc\n"); CHECK(!LiveBenchRequest(&tok) && !LiveBenchCancel());                          // not a number: ignored
        Req("-5\n"); CHECK(!LiveBenchRequest(&tok));
        Req("17000000010 x\n"); CHECK(!LiveBenchRequest(&tok));
        Req("99999999999999999999\n"); CHECK(!LiveBenchRequest(&tok));
        Req("  1700000003  \r\n"); CHECK(LiveBenchRequest(&tok) && tok == 1700000003ll);             // blanks and CRLF are fine
        LiveBenchAck(tok);
        // a BENCH line in live.conf (an older plugin or script) is neither a setting nor a request, and it moves no live value
        Write(conf, "REV=30\nBENCH=1700000009\nTARGET_FPS=75\n"); Touch(conf, 60); Poll();
        CHECK(!LiveBenchRequest(&tok) && Live().target_fps == 75.0f);
        const unsigned long long gen1 = LiveGeneration();
        Write(conf, "REV=31\nBENCH=1700000010\nTARGET_FPS=75\n"); Touch(conf, 62); Poll();
        CHECK(!LiveBenchRequest(&tok) && LiveGeneration() == gen1);
        // a NUL byte, and a FIFO in the place of bench.req (open() must not block the game)
        Write(req, std::string("1700000051\0x\n", 13)); Touch(req, t += 2); Poll(); CHECK(!LiveBenchRequest(&tok));
        unlink(req.c_str()); CHECK(mkfifo(req.c_str(), 0600) == 0);
        alarm(10); Poll(); alarm(0); CHECK(!LiveBenchRequest(&tok));
        unlink(req.c_str());
        // the file is read only when it is the user's own regular file: a symlink is not followed
        unlink(req.c_str());
        Poll(); CHECK(!LiveBenchRequest(&tok));
        const std::string target = std::string(dir) + "/elsewhere";
        Write(target, "1700000099\n");
        CHECK(symlink(target.c_str(), req.c_str()) == 0); Poll(); CHECK(!LiveBenchRequest(&tok));
        unlink(req.c_str()); unlink(target.c_str());
        Write(conf, "REV=32\nTARGET_FPS=75\n"); Touch(conf, 64); Poll();
    }

    // REV is only logged; a value that is not a whole number is ignored and said so, it is not read as a prefix or as 0 or as a wrapped number
    Write(conf, "REV=40\nTARGET_FPS=75\n"); Touch(conf, 66); Poll();
    Write(conf, "REV=12abc\nTARGET_FPS=76\n"); Touch(conf, 68); Poll();
    Write(conf, "REV=99999999999999999999999\nTARGET_FPS=77\n"); Touch(conf, 70); Poll();
    Write(conf, "REV=-5\nTARGET_FPS=78\n"); Touch(conf, 72); Poll();
    CHECK(L.target_fps == 78.0f);                                 // the settings in those files still applied
    Write(conf, "REV=41\nTARGET_FPS=75\n"); Touch(conf, 74); Poll();

    unlink(conf.c_str());                                         // removing the file reverts everything
    Poll();
    CHECK(L.target_fps == 90.0f && L.enabled == 1 && L.cmd_buffers == 8 && L.synth_margin == 0.15f);

    const std::string text = Slurp(log);
    CHECK(text.find("startup setting EXTRAPOLATE=1") != std::string::npos);
    CHECK(text.find("session start") != std::string::npos);
    CHECK(text.find("REV=\"12abc\" is not a whole number") != std::string::npos && text.find("REV=\"99999999999999999999999\" is not a whole number") != std::string::npos && text.find("REV=\"-5\" is not a whole number") != std::string::npos);
    CHECK(text.find("REV 40,") != std::string::npos && text.find("REV 41,") != std::string::npos);            // the last good number stays, then the next good one
    CHECK(text.find("CHANGE TARGET_FPS: 90 -> 60") != std::string::npos);
    CHECK(text.find("CHANGE ENABLED: 1 -> 0") != std::string::npos);
    CHECK(text.find("clamped to the range") != std::string::npos);
    CHECK(text.find("unknown key \"NONSENSE\"") != std::string::npos);
    CHECK(text.find("unknown key \"OPTICAL_FLOW\"") == std::string::npos && text.find("unknown key \"INTERPOLATE\"") == std::string::npos && text.find("unknown key \"WARP\"") == std::string::npos);          // start-of-game keys are not errors
    CHECK(text.find("CHANGE MIN_GAIN") == std::string::npos);              // 1.01 vs 1.01f is not a change
    CHECK(text.find("no '='") != std::string::npos);
    CHECK(text.find("not a valid value") != std::string::npos);
    CHECK(text.find("live.conf removed") != std::string::npos);
    {   // the log is plain lines: no control character but the line ends, whatever the settings file contained
        bool clean = true; for (char ch : text) if (ch != '\n' && (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f)) clean = false;
        CHECK(clean);
        CHECK(text.find("contains a NUL byte") != std::string::npos);
        CHECK(text.find("bigger than 65536 bytes") != std::string::npos);
        size_t bogus = 0; for (size_t pos = 0; (pos = text.find("unknown key \"BOGUS", pos)) != std::string::npos; ++pos) ++bogus;
        CHECK(bogus > 0 && bogus <= 10);                                                    // the notes of one file are capped
        CHECK(text.find("more notes about this file not shown") != std::string::npos);
    }
    CHECK(text.find("BENCH in live.conf is not used any more") != std::string::npos);
    CHECK(text.find("bench.req: request 1700000001") != std::string::npos && text.find("is not a request number") != std::string::npos);
    CHECK(text.find("unknown key \"BENCH\"") == std::string::npos);
    CHECK(text.find("back to the startup value") != std::string::npos);

    {   // files the layer writes are private: folder 0700, status.json 0600, and no HOME / XDG_CONFIG_HOME means no files at all
        char home[] = "/tmp/vkihome_XXXXXX";
        CHECK(mkdtemp(home) != nullptr);
        unsetenv("VKIFRAME_LIVE_CONF"); unsetenv("VKIFRAME_LIVE_LOG");
        setenv("XDG_CONFIG_HOME", home, 1);
        LiveWriteStatus(30, 90, 2, "interp", 90.0);
        struct stat sd, sf;
        const std::string cd = std::string(home) + "/vkiframes";
        CHECK(stat(cd.c_str(), &sd) == 0 && (sd.st_mode & 0777) == 0700);
        CHECK(stat((cd + "/status.json").c_str(), &sf) == 0 && (sf.st_mode & 0777) == 0600);
        unlink((cd + "/status.json").c_str()); rmdir(cd.c_str());
        unsetenv("XDG_CONFIG_HOME"); unsetenv("HOME");
        LiveWriteStatus(30, 90, 2, "interp", 90.0);                 // nowhere to write: nothing happens, and nothing lands in /tmp
        CHECK(access("/tmp/vkiframes/status.json", F_OK) != 0);
        rmdir(home);
    }
    unlink(log.c_str()); rmdir(dir);
    std::printf(g_fail ? "live config: FAILED\n" : "live config: ok\n");
    return g_fail ? 1 : 0;
}
