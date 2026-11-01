// Swapchain capacity in the live settings: the formula C = min(limit, max(MAX_SYNTHETIC requested at start, 4)) with live.conf over the environment, and the live
// clamp: MAX_SYNTHETIC never goes above what the swapchain was built for, with one log line per (value, capacity), also across rewrites of live.conf. No GPU.
#include "live_config.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static std::string Slurp(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
static int Count(const std::string& t, const std::string& n) { int c = 0; for (size_t p = t.find(n); p != std::string::npos; p = t.find(n, p + 1)) ++c; return c; }
static uint64_t g_now = 1'000'000'000ull;
static void Write(const std::string& p, const std::string& t) { { std::ofstream f(p, std::ios::trunc); f << t; } struct timespec ts[2]; static int n = 0; clock_gettime(CLOCK_REALTIME, &ts[0]); ts[0].tv_sec += 10 * ++n; ts[1] = ts[0]; utimensat(AT_FDCWD, p.c_str(), ts, 0); }
static void Poll() { g_now += 1'100'000'000ull; LivePoll(g_now); }

int main() {
    char dir[] = "/tmp/vkicaplive_XXXXXX"; if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/live.conf", log = std::string(dir) + "/live.log";
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1); setenv("VKIFRAME_LIVE_LOG", log.c_str(), 1); setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("VKIFRAME_EXPERIMENTAL", "1", 1);                         // the limit is 10
    LiveSettings& L = Live();
    CHECK(LiveMaxSyntheticLimit() == 10);

    // the formula: no file, the environment's request
    CHECK(LiveSynthCapacityFor(1) == 4 && LiveSynthCapacityFor(3) == 4 && LiveSynthCapacityFor(4) == 4 && LiveSynthCapacityFor(8) == 8 && LiveSynthCapacityFor(10) == 10);
    Write(conf, "VKIFRAME_MAX_SYNTHETIC=7\n");
    CHECK(LiveRequestedMaxSynthetic() == 7 && LiveSynthCapacityFor(3) == 7);                      // live.conf over the environment
    Write(conf, "MAX_SYNTHETIC=2\nFRAMEGEN_TARGET_FPS=60\n");
    CHECK(LiveRequestedMaxSynthetic() == 2 && LiveSynthCapacityFor(9) == 4);                      // the file says 2: wins over the environment's 9, C stays at least 4
    Write(conf, "MAX_SYNTHETIC=99\n");
    CHECK(LiveRequestedMaxSynthetic() == 10 && LiveSynthCapacityFor(3) == 10);                    // clamped to the hard limit
    Write(conf, "# nothing here\nTARGET_FPS=60\n");
    CHECK(LiveRequestedMaxSynthetic() == 0);
    unlink(conf.c_str());
    CHECK(LiveRequestedMaxSynthetic() == 0);

    // the live clamp: the swapchain was built for 4
    LiveSetSynthCapacity(4);
    CHECK(LiveSynthCapacity() == 4);
    Write(conf, "MAX_SYNTHETIC=6\n"); Poll();
    CHECK(L.max_synthetic == 4);
    Write(conf, "MAX_SYNTHETIC=6\nTARGET_FPS=75\n"); Poll();                                      // rewritten with another change: still 4, still one line
    CHECK(L.max_synthetic == 4 && L.target_fps == 75.0f);
    CHECK(Count(Slurp(log), "above the 4 this game's swapchain was built for") == 1);
    Write(conf, "MAX_SYNTHETIC=7\n"); Poll();                                                     // another value: one more line
    CHECK(L.max_synthetic == 4 && Count(Slurp(log), "above the 4 this game's swapchain was built for") == 2);
    Write(conf, "MAX_SYNTHETIC=3\n"); Poll();                                                     // within the capacity: taken as it is
    CHECK(L.max_synthetic == 3);
    // a smaller capacity (fewer images granted than wanted) clamps the running value at once
    Write(conf, "MAX_SYNTHETIC=4\n"); Poll();
    CHECK(L.max_synthetic == 4);
    LiveSetSynthCapacity(2);
    CHECK(L.max_synthetic == 2 && Count(Slurp(log), "MAX_SYNTHETIC 4 is above the 2 this game's swapchain was built for") == 1);
    // unknown capacity (no frame holding): only the limit applies
    LiveSetSynthCapacity(0);
    Write(conf, "MAX_SYNTHETIC=9\n"); Poll();
    CHECK(L.max_synthetic == 9);

    if (g_fail) std::printf("%s\n", Slurp(log).c_str()); std::string cmd = std::string("rm -rf ") + dir; if (std::system(cmd.c_str()) != 0) ++g_fail;
    std::printf(g_fail ? "capacity live: FAILED\n" : "capacity live: ok\n");
    return g_fail ? 1 : 0;
}
