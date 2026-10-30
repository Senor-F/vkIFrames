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

    unlink(conf.c_str());                                         // removing the file reverts everything
    Poll();
    CHECK(L.target_fps == 90.0f && L.enabled == 1);

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
