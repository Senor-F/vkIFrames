// Measurement logic: percentiles, derived values, the benchmark schedule and bench.json, the BENCH request through live.conf, privacy of the files.
#include "live_config.hpp"
#include "stats.hpp"
#include "vk_iframes_layer.hpp"

#include <cmath>
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
#define NEAR(a, b, e) do { if (std::fabs(double(a) - double(b)) > (e)) { std::printf("FAIL line %d: %s = %g, want %g\n", __LINE__, #a, double(a), double(b)); ++g_fail; } } while (0)

static std::string Slurp(const std::string& path) { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void Write(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc); f << text; }
    std::rename(tmp.c_str(), path.c_str());
}
static uint64_t g_now = 1'000'000'000ull;
static void Poll() { g_now += 1'100'000'000ull; LivePoll(g_now); }
static int g_touch = 0;
static void Put(const std::string& path, const std::string& text) { Write(path, text); g_touch += 5; struct timespec ts[2]; clock_gettime(CLOCK_REALTIME, &ts[0]); ts[0].tv_sec += g_touch; ts[1] = ts[0]; utimensat(AT_FDCWD, path.c_str(), ts, 0); }

// What the game does: 60 fps with the layer off, 45 native frames and 90 shown (one generated each) with it on.
static stats::Record Second(bool layer_on) {
    stats::Record r;
    stats::FrameTimes ft;
    const int n = layer_on ? 45 : 60;
    for (int i = 0; i < n; ++i) ft.Add(layer_on ? 22.0 + (i == 0 ? 8.0 : 0.0) : 16.0 + (i == 0 ? 4.0 : 0.0));
    r.secs = 1.0; r.mode = layer_on ? "interp" : "off"; r.target_fps = 90; r.refresh_hz = 90;
    r.native_fps = n; r.shown_fps = layer_on ? 90 : 60;
    r.cpu_game_ms = layer_on ? 0.5 : 0.01; r.presenter_core = layer_on ? 0.2 : 0.0;
    r.wait_acquire_ms = layer_on ? 0.3 : 0.0; r.wait_lock_ms = layer_on ? 0.1 : 0.0;
    if (layer_on) { r.flow_ms = 4.0; r.pairs_per_s = 45; r.interp_ms = 1.0; r.generated_per_s = 45; }
    stats::FillDerived(r, ft);
    return r;
}

int main() {
    // ---- percentiles: nearest rank, counting from 1 ----
    {
        std::vector<double> v; for (int i = 100; i >= 1; --i) v.push_back(i);
        NEAR(stats::Percentile(v, 0.95), 95, 0); NEAR(stats::Percentile(v, 0.99), 99, 0); NEAR(stats::Percentile(v, 1.0), 100, 0); NEAR(stats::Percentile(v, 0.0), 1, 0);
        NEAR(stats::Percentile({5.0}, 0.95), 5, 0); NEAR(stats::Percentile({4, 1, 3, 2}, 0.95), 4, 0); NEAR(stats::Percentile({}, 0.95), 0, 0);
        NEAR(stats::Percentile({10, 20}, 0.5), 10, 0);
    }
    // ---- derived values of one second ----
    {
        stats::Record r = Second(true);
        CHECK(r.frames == 45);
        NEAR(r.ft_mean, (44 * 22.0 + 30.0) / 45.0, 1e-9);
        NEAR(r.ft_max, 30.0, 0); NEAR(r.ft_p95, 22.0, 0); NEAR(r.ft_p99, 30.0, 0);    // ceil(0.95*45) = 43rd of 45 sorted: 22.0; ceil(0.99*45) = 45th: the one 30 ms frame
        NEAR(r.gpu_share, (45 * 4.0 + 45 * 1.0) / 1000.0, 1e-12);                       // 0.225 of the GPU
        const std::string j = stats::RecordJson(r, 1700000000);
        CHECK(j.find("\"gpu_share\":0.22500") != std::string::npos && j.find("\"mode\":\"interp\"") != std::string::npos);
        CHECK(j.front() == '{' && j.back() == '}' && j.find('\n') == std::string::npos);
        CHECK(j.find('/') == std::string::npos);                                         // no path in a record
    }
    // ---- the schedule and the derived numbers ----
    {
        stats::Bench b;
        b.Start(1, 90, 4, 60, 1.01, 1);
        int off_switches = 0, on_switches = 0, seconds = 0, enabled = 1;
        std::string labels;
        while (b.Active() && seconds < 1000) {
            stats::Record r = Second(enabled != 0);
            const int next = b.OnSecond(r);
            ++seconds;
            if (seconds <= 30) CHECK(r.bench == "warmup" && enabled == 1);
            if (next != enabled) { (next ? on_switches : off_switches)++; enabled = next; }
        }
        CHECK(seconds == 30 + 6 * 60);
        CHECK(b.GetState() == stats::Bench::State::Complete);
        CHECK(off_switches == 3 && on_switches == 3 + 0);        // off at 30, 150, 270; on at 90, 210, 330; the end returns the user's value (1)
        const auto off = b.Aggregate(false), on = b.Aggregate(true);
        CHECK(off.seconds == 3 * 58 && on.seconds == 3 * 58);    // 2 settle seconds left out of every segment
        NEAR(off.ft_mean, (59 * 16.0 + 20.0) / 60.0, 1e-9);
        NEAR(on.ft_mean, (44 * 22.0 + 30.0) / 45.0, 1e-9);
        NEAR(on.gpu_share, 0.225, 1e-9); NEAR(on.gpu_flow_share, 0.18, 1e-9); NEAR(on.gpu_interp_share, 0.045, 1e-9);
        NEAR(on.flow_ms, 4.0, 1e-9); NEAR(on.interp_ms, 1.0, 1e-9); NEAR(off.gpu_share, 0.0, 0);
        NEAR(on.cpu_game_ms, 0.5, 1e-9); NEAR(on.presenter_core, 0.2, 1e-9); NEAR(on.wait_game_ms, 0.4, 1e-9);
        const std::string j = b.Json();
        const double cost = on.ft_mean - off.ft_mean;
        char want[80]; std::snprintf(want, sizeof want, "\"layer_cost_mean_ms\":%.3f", cost);
        CHECK(j.find("\"status\":\"complete\"") != std::string::npos && j.find(want) != std::string::npos);
        std::snprintf(want, sizeof want, "\"async_gain_upper_bound_ms\":%.3f", cost);
        CHECK(j.find(want) != std::string::npos);
        CHECK(j.find("\"flow_share_of_gpu\":0.8000") != std::string::npos);
        CHECK(j.find("\"base_fps_cap_kept_while_off\":true") != std::string::npos);
        CHECK(b.Restore() == 1);
    }
    // labels and the first seconds of a run
    {
        stats::Bench b; b.Start(0, 90, 4, 60, 1.01, 1);
        std::string seq; int en = 1;
        for (int i = 0; i < 100 && b.Active(); ++i) {
            stats::Record r = Second(en != 0); en = b.OnSecond(r);
            if (i == 0 || i == 29 || i == 30 || i == 31 || i == 32 || i == 33 || i == 89 || i == 90 || i == 91 || i == 92) seq += r.bench + (en ? "+" : "-") + " ";
        }
        // the record at index i carries the label of its own second; en is what the next second runs with
        CHECK(seq == "warmup+ warmup- settle- settle- off- off- off+ settle+ settle+ on+ ");
        stats::Bench c; c.Start(0, 90, 4, 60, 1.01, 1);                  // the user's ENABLED was 0: it comes back at the end
        int e = 1, s = 0; while (c.Active() && s++ < 1000) { stats::Record r = Second(e != 0); e = c.OnSecond(r); }
        CHECK(e == 0 && c.Restore() == 0);
    }
    // an aborted run says so and has no derived numbers
    {
        stats::Bench b; b.Start(1, 90, 4, 60, 1.01, 1);
        int e = 1; for (int i = 0; i < 100; ++i) { stats::Record r = Second(e != 0); e = b.OnSecond(r); }
        b.Abort("a setting was changed during the run");
        const std::string j = b.Json();
        CHECK(b.GetState() == stats::Bench::State::Aborted && j.find("\"status\":\"aborted\"") != std::string::npos && j.find("\"derived\":null") != std::string::npos);
        CHECK(j.find("a setting was changed") != std::string::npos);
    }

    // ---- through live.conf: a BENCH request, the run, the files ----
    char dir[] = "/tmp/vkistats_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/vkiframes/live.conf";
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1);
    setenv("VKIFRAME_LIVE_LOG", (std::string(dir) + "/vkiframes/live.log").c_str(), 1);
    ::mkdir((std::string(dir) + "/vkiframes").c_str(), 0700);
    Poll();                                                              // no file yet
    CHECK(!StatsOn() && !StatsWantsSecond());                            // off: nothing is asked of the layer
    Put(conf, "REV=1\nENABLED=1\nBASE_FPS_CAP=60\n");
    Poll();
    CHECK(!StatsWantsSecond());
    Put(conf, "REV=2\nENABLED=1\nBASE_FPS_CAP=60\nBENCH=1700000001\n");
    Poll();
    CHECK(StatsWantsSecond() && !StatsOn());                             // a request is waiting
    {
        int seconds = 0; int enabled_seen_off = 0;
        stats::Record r = Second(true);
        StatsOnSecond(r, g_now);                                         // starts the run; this second is not part of it
        CHECK(StatsBenchActive() && StatsOn() && Live().enabled.load() == 1);
        while (StatsBenchActive() && seconds < 1000) {
            stats::Record q = Second(Live().enabled.load() != 0);
            StatsOnSecond(q, g_now);
            ++seconds;
            if (Live().enabled.load() == 0) ++enabled_seen_off;
        }
        CHECK(seconds == 30 + 6 * 60 && enabled_seen_off > 0);
        CHECK(Live().enabled.load() == 1);                               // restored
        CHECK(!StatsBenchActive() && !StatsOn());
        CHECK(std::string(StatsBenchLabel()) == "done");
        const std::string bj = dir + std::string("/vkiframes/bench.json"), sj = dir + std::string("/vkiframes/stats.jsonl");
        struct stat st;
        CHECK(stat(bj.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(stat(sj.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(stat((std::string(dir) + "/vkiframes").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
        const std::string j = Slurp(bj);
        CHECK(j.find("\"status\":\"complete\"") != std::string::npos && j.find("\"derived\":{") != std::string::npos);
        CHECK(j.find(dir) == std::string::npos && j.find("/home") == std::string::npos);         // no path anywhere
        // one line per second of the run, each a single JSON object
        std::ifstream f(sj); std::string line; int lines = 0, labelled = 0;
        while (std::getline(f, line)) { ++lines; CHECK(line.front() == '{' && line.back() == '}'); if (line.find("\"bench\":\"\"") == std::string::npos) ++labelled; }
        CHECK(lines == seconds && labelled == seconds);
    }
    // the same token does not start a second run; a new one does; a setting change stops it and keeps the user's value
    Poll(); CHECK(!StatsWantsSecond());
    Put(conf, "REV=3\nENABLED=1\nBASE_FPS_CAP=60\nBENCH=1700000002\n");
    Poll(); CHECK(StatsWantsSecond());
    {
        stats::Record r = Second(true); StatsOnSecond(r, g_now);
        CHECK(StatsBenchActive());
        for (int i = 0; i < 40; ++i) { stats::Record q = Second(Live().enabled.load() != 0); StatsOnSecond(q, g_now); }    // warm-up over, an "off" segment running
        CHECK(Live().enabled.load() == 0);
        Put(conf, "REV=4\nENABLED=1\nBASE_FPS_CAP=60\nBENCH=1700000002\nTARGET_FPS=75\n");
        Poll();
        stats::Record q = Second(false); StatsOnSecond(q, g_now);
        CHECK(!StatsBenchActive());
        CHECK(Live().enabled.load() == 1 && Live().target_fps.load() == 75.0f);       // the file's values stand
        CHECK(Slurp(std::string(dir) + "/vkiframes/bench.json").find("a setting was changed") != std::string::npos);
        CHECK(std::string(StatsBenchLabel()) == "aborted");
    }
    // leaving the game stops a run and restores ENABLED
    Put(conf, "REV=5\nENABLED=1\nBASE_FPS_CAP=60\nBENCH=1700000003\n");
    Poll();
    {
        stats::Record r = Second(true); StatsOnSecond(r, g_now);
        for (int i = 0; i < 40; ++i) { stats::Record q = Second(Live().enabled.load() != 0); StatsOnSecond(q, g_now); }
        CHECK(StatsBenchActive() && Live().enabled.load() == 0);
        StatsBenchAbort("the game closed its swapchain");
        CHECK(!StatsBenchActive() && Live().enabled.load() == 1);
    }
    // STATS=1 alone: records, no run
    Put(conf, "REV=6\nSTATS=1\n");
    Poll();
    CHECK(StatsOn() && !StatsBenchActive());
    {
        const std::string sj = dir + std::string("/vkiframes/stats.jsonl");
        std::ifstream f0(sj); std::string l; int before = 0; while (std::getline(f0, l)) ++before;
        stats::Record r = Second(true); StatsOnSecond(r, g_now);
        std::ifstream f1(sj); int after = 0; while (std::getline(f1, l)) ++after;
        CHECK(after == before + 1);
    }
    Put(conf, "REV=7\nSTATS=0\n");
    Poll(); CHECK(!StatsOn());

    std::printf(g_fail ? "stats: %d check(s) FAILED\n" : "stats tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
