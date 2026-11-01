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
    setenv("VKIFRAME_WARP", "1", 1);                              // the old name of INTERPOLATE still works

    Write(conf, "REV=0\nFRAMEGEN_EXTRAPOLATE=1\nVKIFRAME_UI_MASK_THRESHOLD=0.03\nTARGET_FPS=75\n");
    CHECK(EnvVar("EXTRAPOLATE") && std::string(EnvVar("EXTRAPOLATE")) == "1");          // startup-only keys come from the file
    CHECK(EnvVar("UI_MASK_THRESHOLD") && std::string(EnvVar("UI_MASK_THRESHOLD")) == "0.03");
    CHECK(!EnvVar("FLOW_LEVEL"));                          // not in the file / not a startup key
    unlink(conf.c_str());

    {   // default target: 90 on the Deck OLED, 60 elsewhere
        const std::string dmi = std::string(dir) + "/dmi";
        Write(dmi, "Galileo\n"); CHECK(DefaultTargetFps(dmi.c_str()) == 90.0f);
        Write(dmi, "Jupiter\n"); CHECK(DefaultTargetFps(dmi.c_str()) == 60.0f);
        Write(dmi, "MS-7E12\n"); CHECK(DefaultTargetFps(dmi.c_str()) == 60.0f);
        CHECK(DefaultTargetFps("/nonexistent/dmi") == 60.0f);
        unlink(dmi.c_str());
    }
    LiveSettings& L = Live();
    CHECK(GetConfig().interpolate);
    Poll();                                                       // no file: environment values
    CHECK(L.enabled == 1 && L.target_fps == 90.0f && L.base_fps_cap == 45.0f && L.max_synthetic == 3);

    Write(conf, "# test\nREV=1\nFRAMEGEN_TARGET_FPS=60\nVKIFRAME_MAX_SYNTHETIC=2\nWARP_BIDIR=1\nENABLED=1\n");
    Poll();
    CHECK(L.target_fps == 60.0f && L.max_synthetic == 2 && L.interp_bidir == 1 && L.base_fps_cap == 45.0f);   // missing keys keep the environment value

    Write(conf, "REV=2\nTARGET_FPS=9999\nMAX_SYNTHETIC=9\nMIN_GAIN=abc\nPHASE_OFFSET=-3\nBASE_FPS_CAP=500\nNONSENSE=1\nWARP=1\nINTERPOLATE=1\nWARP_BIDIR=0\nFLOW_LEVEL=1\nno equals sign\nENABLED=0\n"); Touch(conf, 5);
    Poll();
    CHECK(L.target_fps == 1000.0f);                                // clamped to the schema range
    CHECK(L.max_synthetic == kLiveMaxSyntheticNormal);   // without EXPERIMENTAL at game start the cap is 4
    CHECK(L.min_gain == 1.01f);                                   // invalid value: the previous (startup) value stays
    CHECK(L.phase_offset == 0.0f);
    CHECK(L.base_fps_cap == 90.0f);
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
        // the file is read only when it is the user's own regular file: a symlink is not followed
        unlink(req.c_str());
        Poll(); CHECK(!LiveBenchRequest(&tok));
        const std::string target = std::string(dir) + "/elsewhere";
        Write(target, "1700000099\n");
        CHECK(symlink(target.c_str(), req.c_str()) == 0); Poll(); CHECK(!LiveBenchRequest(&tok));
        unlink(req.c_str()); unlink(target.c_str());
        Write(conf, "REV=32\nTARGET_FPS=75\n"); Touch(conf, 64); Poll();
    }

    unlink(conf.c_str());                                         // removing the file reverts everything
    Poll();
    CHECK(L.target_fps == 90.0f && L.enabled == 1 && L.cmd_buffers == 8 && L.synth_margin == 0.15f);

    const std::string text = Slurp(log);
    CHECK(text.find("startup setting EXTRAPOLATE=1") != std::string::npos);
    CHECK(text.find("session start") != std::string::npos);
    CHECK(text.find("CHANGE TARGET_FPS: 90 -> 60") != std::string::npos);
    CHECK(text.find("CHANGE ENABLED: 1 -> 0") != std::string::npos);
    CHECK(text.find("clamped to the range") != std::string::npos);
    CHECK(text.find("unknown key \"NONSENSE\"") != std::string::npos);
    CHECK(text.find("unknown key \"INTERPOLATE\"") == std::string::npos);          // start-of-game keys are not errors
    CHECK(text.find("CHANGE MIN_GAIN") == std::string::npos);              // 1.01 vs 1.01f is not a change
    CHECK(text.find("no '='") != std::string::npos);
    CHECK(text.find("not a valid value") != std::string::npos);
    CHECK(text.find("live.conf removed") != std::string::npos);
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
