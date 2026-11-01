// overrides.conf: precedence default < environment < live.conf < overrides.conf, the base rule (a panel change of a key wins over an override set
// against another value, also across a restart), a bad overrides.conf (symlink, directory, unreadable, malformed) logged once, BENCH ignored, the
// status.json "effective" and "source" fields, and the benchmark refusal. One scenario per process (the layer's state is global): argv[1].
#include "live_config.hpp"
#include "stats.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static std::string g_dir, g_conf, g_ov, g_log;
static int g_tick = 0;
static uint64_t g_now = 1'000'000'000ull;

static void Write(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc); f << text; }
    std::rename(tmp.c_str(), path.c_str());
    struct timespec ts[2]; clock_gettime(CLOCK_REALTIME, &ts[0]); ts[0].tv_sec += 100 + 2 * (++g_tick); ts[1] = ts[0];   // a new mtime for every write
    utimensat(AT_FDCWD, path.c_str(), ts, 0);
}
static std::string Slurp(const std::string& path) { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); return s.str(); }
static int Count(const std::string& text, const std::string& needle) { int n = 0; for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++n; return n; }
static void Poll() { g_now += 1'100'000'000ull; LivePoll(g_now); }
static bool Src(const char* key, const char* want) { const char* s = LiveSourceOf(key); return s && std::strcmp(s, want) == 0; }
static void Setup(const char* tag) {
    char dir[64]; std::snprintf(dir, sizeof dir, "/tmp/vkiov_%s_XXXXXX", tag);
    g_dir = mkdtemp(dir) ? dir : "";
    g_conf = g_dir + "/live.conf"; g_ov = g_dir + "/overrides.conf"; g_log = g_dir + "/live.log";
    setenv("VKIFRAME_LIVE_CONF", g_conf.c_str(), 1);
    setenv("VKIFRAME_LIVE_LOG", g_log.c_str(), 1);
    setenv("VKIFRAME_TARGET_FPS", "90", 1);
    setenv("VKIFRAME_BASE_FPS_CAP", "45", 1);
    setenv("XDG_CONFIG_HOME", g_dir.c_str(), 1);               // status.json, bench.json go to <dir>/vkiframes
}

static void ScenarioMain() {
    LiveSettings& L = Live();
    Poll();                                                    // no files
    CHECK(L.target_fps == 90.0f && Src("TARGET_FPS", "env") && Src("MAX_SYNTHETIC", "default") && LiveOverrideKeys().empty());

    Write(g_ov, "REV=1\nTARGET_FPS=75 ;base=absent ;t=1\n");    // set with no live.conf: base is "absent"
    Poll();
    CHECK(L.target_fps == 75.0f && Src("TARGET_FPS", "overrides.conf") && LiveOverrideKeys() == "TARGET_FPS");

    Write(g_conf, "REV=1\nTARGET_FPS=90\nENABLED=1\n");          // the plugin writes its file: TARGET_FPS at the layer's own (environment) value
    Poll();
    CHECK(L.target_fps == 75.0f && Src("TARGET_FPS", "overrides.conf"));                 // base=absent compares with the lower value: not a change

    Write(g_conf, "REV=2\nTARGET_FPS=60\nENABLED=1\n");          // a panel action
    Poll();
    CHECK(L.target_fps == 60.0f && Src("TARGET_FPS", "live.conf") && LiveOverrideKeys().empty());

    Write(g_conf, "REV=3\nTARGET_FPS=60\nENABLED=0\n");          // another key changes, TARGET_FPS rewritten unchanged
    Poll();
    CHECK(L.target_fps == 60.0f && L.enabled == 0 && Src("TARGET_FPS", "live.conf"));

    Write(g_conf, "REV=4\nTARGET_FPS=90\nENABLED=0\n");          // back to the base value: the override applies again
    Poll();
    CHECK(L.target_fps == 75.0f && Src("TARGET_FPS", "overrides.conf"));

    // numeric base: ctl set BASE_FPS_CAP=30 while live.conf said 60
    Write(g_conf, "REV=5\nTARGET_FPS=90\nENABLED=0\nBASE_FPS_CAP=60\n");
    Write(g_ov, "REV=2\nTARGET_FPS=75 ;base=absent\nBASE_FPS_CAP=30 ;base=60 ;t=2\n");
    Poll();
    CHECK(L.base_fps_cap == 30.0f && Src("BASE_FPS_CAP", "overrides.conf"));
    Write(g_conf, "REV=6\nTARGET_FPS=90\nENABLED=0\nBASE_FPS_CAP=40\n");               // panel moves it
    Poll();
    CHECK(L.base_fps_cap == 40.0f && Src("BASE_FPS_CAP", "live.conf"));
    Write(g_ov, "REV=3\nTARGET_FPS=75 ;base=absent\nBASE_FPS_CAP=30 ;base=40 ;t=3\n");   // ctl sets it again: records the new base
    Poll();
    CHECK(L.base_fps_cap == 30.0f && Src("BASE_FPS_CAP", "overrides.conf"));

    // removal reverts to the live.conf values (not to the environment)
    unlink(g_ov.c_str());
    Poll();
    CHECK(L.base_fps_cap == 40.0f && L.target_fps == 90.0f && Src("BASE_FPS_CAP", "live.conf") && LiveOverrideKeys().empty());

    const std::string text = Slurp(g_log);
    CHECK(text.find("CHANGE TARGET_FPS: 90 -> 75 (overrides.conf)") != std::string::npos);
    CHECK(Count(text, "override TARGET_FPS superseded by live.conf (panel)") == 1);
    CHECK(Count(text, "override TARGET_FPS applies again") == 1);
    CHECK(text.find("overrides.conf removed: live.conf values apply") != std::string::npos);
}

static void ScenarioRestart() {     // the files are as a previous game left them: the panel changed BASE_FPS_CAP after ctl set it
    Write(g_conf, "REV=9\nBASE_FPS_CAP=85\nTARGET_FPS=90\n");
    Write(g_ov, "REV=4\nBASE_FPS_CAP=30 ;base=60 ;t=5\nTARGET_FPS=50 ;base=90 ;t=5\n");
    LiveSettings& L = Live();
    Poll();                                                    // first load of a new run
    CHECK(L.base_fps_cap == 85.0f && Src("BASE_FPS_CAP", "live.conf"));        // the panel's action survives the restart
    CHECK(L.target_fps == 50.0f && Src("TARGET_FPS", "overrides.conf"));       // the other key's override still applies
    const std::string text = Slurp(g_log);
    CHECK(text.find("override BASE_FPS_CAP superseded by live.conf (panel)") != std::string::npos);
}

static void ScenarioHandWritten() {  // a line without ;base: the base is the live.conf value of the key at the first load
    Write(g_conf, "TARGET_FPS=70\n");
    Write(g_ov, "TARGET_FPS=50\nBASE_FPS_CAP=20\n");
    LiveSettings& L = Live();
    Poll();
    CHECK(L.target_fps == 50.0f && L.base_fps_cap == 20.0f && Src("TARGET_FPS", "overrides.conf"));
    Write(g_conf, "TARGET_FPS=70\nBASE_FPS_CAP=60\n");            // K absent at the first load, live.conf now has it: superseded
    Poll();
    CHECK(L.target_fps == 50.0f && L.base_fps_cap == 60.0f && Src("BASE_FPS_CAP", "live.conf"));
    Write(g_conf, "TARGET_FPS=71\nBASE_FPS_CAP=60\n");            // changed from the first-load value: superseded
    Poll();
    CHECK(L.target_fps == 71.0f && Src("TARGET_FPS", "live.conf"));
}

static void ScenarioNoFilesAtStart() {   // nothing at start; live.conf appears later with a value, then an override: the override (newer, set against it) applies
    LiveSettings& L = Live();
    Poll();
    Write(g_conf, "TARGET_FPS=60\n");
    Poll();
    CHECK(L.target_fps == 60.0f && Src("TARGET_FPS", "live.conf"));
    Write(g_ov, "TARGET_FPS=45 ;base=60\n");
    Poll();
    CHECK(L.target_fps == 45.0f && Src("TARGET_FPS", "overrides.conf"));
}

static void ScenarioBadFiles() {
    LiveSettings& L = Live();
    Poll();
    // a symlink: ignored, one log line however many polls
    Write(g_dir + "/real.conf", "TARGET_FPS=33 ;base=absent\n");
    CHECK(symlink((g_dir + "/real.conf").c_str(), g_ov.c_str()) == 0);
    for (int i = 0; i < 3; ++i) Poll();
    CHECK(L.target_fps == 90.0f && Src("TARGET_FPS", "env"));
    CHECK(Count(Slurp(g_log), "overrides.conf ignored: it is a symbolic link") == 1);
    unlink(g_ov.c_str());
    // a directory
    CHECK(mkdir(g_ov.c_str(), 0700) == 0);
    for (int i = 0; i < 3; ++i) Poll();
    CHECK(L.target_fps == 90.0f && Count(Slurp(g_log), "overrides.conf ignored: it is not a regular file") == 1);
    rmdir(g_ov.c_str());
    // unreadable (skipped as root, which can read anything)
    if (geteuid() != 0) {
        Write(g_ov, "TARGET_FPS=33 ;base=absent\n"); chmod(g_ov.c_str(), 0);
        for (int i = 0; i < 3; ++i) Poll();
        CHECK(L.target_fps == 90.0f && Count(Slurp(g_log), "overrides.conf ignored: it cannot be read") == 1);
        chmod(g_ov.c_str(), 0600); unlink(g_ov.c_str());
    }
    // malformed, out of range, BENCH, unknown, start-only keys
    Write(g_ov, "REV=7\nno equals sign\nTARGET_FPS=5000 ;base=absent\nBASE_FPS_CAP=abc\nBENCH=777\nNONSENSE=1\nFLOW_LEVEL=3\nMAX_SYNTHETIC=2 ;base=absent ;t=notanumber\n");
    Poll();
    CHECK(L.target_fps == 1000.0f);                            // clamped to the layer's range
    CHECK(L.base_fps_cap == 45.0f);                            // invalid value ignored
    CHECK(L.bench_token == 0);                                 // BENCH is read from live.conf only
    CHECK(L.max_synthetic == 2);
    const std::string t = Slurp(g_log);
    CHECK(t.find("overrides.conf line 2: TARGET_FPS=5000 clamped") != std::string::npos);
    CHECK(t.find("BENCH is read from live.conf only") != std::string::npos);
    CHECK(t.find("unknown key \"NONSENSE\"") != std::string::npos);
    CHECK(t.find("overrides.conf line 1 ignored (no '=')") != std::string::npos);
}

static void ScenarioStatus() {
    Write(g_conf, "TARGET_FPS=60\n");
    Write(g_ov, "ENABLED=0 ;base=absent\nBASE_FPS_CAP=30 ;base=absent\n");
    Poll();
    LiveWriteStatus(30, 60, 1, "off", 60.0, "");
    const std::string st = Slurp(g_dir + "/vkiframes/status.json");
    CHECK(st.find("\"effective\":{\"ENABLED\":0,\"TARGET_FPS\":60,") != std::string::npos);
    CHECK(st.find("\"BASE_FPS_CAP\":30") != std::string::npos && st.find("\"STATS\":0}") != std::string::npos);
    CHECK(st.find("\"source\":{\"ENABLED\":\"overrides.conf\",\"TARGET_FPS\":\"live.conf\",\"MAX_SYNTHETIC\":\"default\",\"BASE_FPS_CAP\":\"overrides.conf\"") != std::string::npos);
    CHECK(st.find("BENCH") == std::string::npos || st.find("\"bench\":\"\"") != std::string::npos);     // the label field only, no BENCH setting
    CHECK(st.find("\"enabled\":0") != std::string::npos && st.find("\"native_fps\":30.0") != std::string::npos);   // the old fields are still there
    struct stat sf; CHECK(stat((g_dir + "/vkiframes/status.json").c_str(), &sf) == 0 && (sf.st_mode & 0777) == 0600);
}

static void ScenarioBench() {
    LiveSettings& L = Live();
    Write(g_conf, "TARGET_FPS=60\n");
    Write(g_ov, "BASE_FPS_CAP=30 ;base=absent\n");
    Poll();
    CHECK(LiveOverrideKeys() == "BASE_FPS_CAP");
    Write(g_conf, "TARGET_FPS=60\nBENCH=4242\n");                // the plugin asks for a run
    Poll();
    CHECK(L.bench_token == 4242);
    stats::Record r;
    StatsOnSecond(r, 0);
    CHECK(!StatsBenchActive());                                // refused at start
    const std::string bj = Slurp(g_dir + "/vkiframes/bench.json");
    CHECK(bj.find("\"status\":\"aborted\"") != std::string::npos && bj.find("overrides active: BASE_FPS_CAP") != std::string::npos);
    CHECK(bj.find("\"sources\":{\"ENABLED\":\"default\",\"TARGET_FPS\":\"live.conf\"") != std::string::npos && bj.find("\"BASE_FPS_CAP\":\"overrides.conf\"") != std::string::npos);
    CHECK(std::string(StatsBenchLabel()) == "aborted");
    // without overrides the same request starts, and bench.json (written at the end) would echo the sources
    unlink(g_ov.c_str());
    Write(g_conf, "TARGET_FPS=60\nBENCH=4243\n");
    Poll();
    CHECK(LiveOverrideKeys().empty());
    StatsOnSecond(r, 0);
    CHECK(StatsBenchActive());
}

int main(int argc, char** argv) {
    const std::string sc = argc > 1 ? argv[1] : "main";
    Setup(sc.c_str());
    if (sc == "main") ScenarioMain();
    else if (sc == "restart") ScenarioRestart();
    else if (sc == "handwritten") ScenarioHandWritten();
    else if (sc == "nofiles") ScenarioNoFilesAtStart();
    else if (sc == "badfiles") ScenarioBadFiles();
    else if (sc == "status") ScenarioStatus();
    else if (sc == "bench") ScenarioBench();
    else { std::printf("unknown scenario %s\n", sc.c_str()); return 2; }
    std::string cmd = "rm -rf " + g_dir; (void)std::system(cmd.c_str());
    std::printf(g_fail ? "overrides %s: FAILED\n" : "overrides %s: ok\n", sc.c_str());
    return g_fail ? 1 : 0;
}
