// live.log and stats.jsonl are capped (default 8 MiB, a small cap here): a file left over from an earlier run is rotated when opened, a growing one is
// rotated while the layer runs, only one old file is kept, and no stream stays open after the rotations. argv[1] "nofile": live.log cannot be opened.
#include "live_config.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static long long Size(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1; }
static long long Ino(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? (long long)st.st_ino : -1; }
static int OpenFds() { int n = 0; if (DIR* d = opendir("/proc/self/fd")) { while (readdir(d)) ++n; closedir(d); } return n; }
static void Fill(const std::string& p, size_t bytes) { FILE* f = std::fopen(p.c_str(), "w"); std::string line(99, 'x'); line += '\n'; for (size_t i = 0; i < bytes / 100; ++i) std::fputs(line.c_str(), f); std::fclose(f); chmod(p.c_str(), 0600); }

int main(int argc, char** argv) {
    char tmpl[] = "/tmp/vkilogcap_XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "";
    const bool nofile = argc > 1 && std::strcmp(argv[1], "nofile") == 0;
    unsetenv("VKIFRAME_LIVE_LOG"); unsetenv("VKIFRAME_LIVE_CONF");
    const std::string xdg = nofile ? root + "/afile/under" : root;                 // under a regular file: the folder cannot be made
    if (nofile) { FILE* f = std::fopen((root + "/afile").c_str(), "w"); std::fclose(f); }
    setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
    const std::string dir = xdg + "/vkiframes", log = dir + "/live.log", stats = dir + "/stats.jsonl";
    if (nofile) {
        CHECK(!LiveLogOpen() && !LiveLogIsFile());
        std::string cmd = "rm -rf " + root; (void)std::system(cmd.c_str());
        std::printf(g_fail ? "logcap nofile: FAILED\n" : "logcap nofile: ok\n");
        return g_fail ? 1 : 0;
    }
    const long long cap = 10000;
    LiveLogSetCap(cap);
    mkdir(dir.c_str(), 0700);
    Fill(log, 20000); Fill(stats, 20000);                                          // left over from an earlier run, above the cap

    CHECK(LiveLogOpen() && LiveLogIsFile());
    CHECK(Size(log + ".1") == 20000 && Size(log) >= 0 && Size(log) < cap);         // rotated when opened
    CHECK(LiveStatsAppend("first line"));
    CHECK(Size(stats + ".1") == 20000 && Size(stats) > 0 && Size(stats) < cap);    // the same for stats.jsonl, before the first append

    // growth while running: about 8000 lines of ~90 bytes, the cap checked once per 20 lines (the layer checks once a second)
    const int fds0 = OpenFds();
    long long worst = 0; int rotations_seen = 0; long long last_ino = Ino(log + ".1");
    for (int i = 1; i <= 8000; ++i) {
        FG_LOG("native 30.0 fps -> output 60.0 fps (planned) | shown 60.0 fps | filler line %06d to grow the log file a little", i);
        if (i % 20 == 0) {
            LiveLogCap();
            worst = std::max(worst, Size(log));
            if (Ino(log + ".1") != last_ino) { ++rotations_seen; last_ino = Ino(log + ".1"); }      // a new .1 file = one rotation
        }
    }
    CHECK(worst < cap + 20 * 140);                                                 // never more than the cap plus the lines of one check interval
    CHECK(rotations_seen >= 10);                                                   // it really rotated, many times
    CHECK(Size(log + ".1") > 0 && Size(log + ".1") < cap + 20 * 140);
    CHECK(access((log + ".2").c_str(), F_OK) != 0);                                // one old file only
    CHECK(OpenFds() <= fds0 + 2);                                                  // retired streams are closed: no descriptor per rotation
    CHECK(LiveLogIsFile());

    // stats.jsonl: 5000 records of 100 bytes, checked every 60 lines
    const std::string rec(99, 'y');
    long long sworst = 0;
    for (int i = 0; i < 5000; ++i) { LiveStatsAppend(rec); sworst = std::max(sworst, Size(stats)); }
    CHECK(sworst < cap + 61 * 100 && Size(stats + ".1") > 0 && access((stats + ".2").c_str(), F_OK) != 0);
    // the total on disk stays below two caps (plus the check interval) for each file
    CHECK(Size(log) + Size(log + ".1") < 2 * cap + 2 * 20 * 140);
    CHECK(Size(stats) + Size(stats + ".1") < 2 * cap + 2 * 61 * 100);

    std::string cmd = "rm -rf " + root; (void)std::system(cmd.c_str());
    std::printf(g_fail ? "logcap: FAILED\n" : "logcap: ok\n");
    return g_fail ? 1 : 0;
}
