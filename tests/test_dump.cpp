// VKIFRAME_DUMP queue: files written together after the window (0600, times.txt), FAILED when a file cannot be written,
// pending items dropped with a log line at teardown. No GPU, no Vulkan calls.
#include "dump_queue.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static std::string Slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main() {
    char tmpl[] = "/tmp/vkiframes_dumptest_XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string logp = dir + "/log.txt";
    g_fg_log = std::fopen(logp.c_str(), "w+");
    const uint8_t px[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto log = [&] { std::fflush(g_fg_log); return Slurp(logp); };

    {   // M=5: nothing is written before the last frame and its synthetic frames are in
        DumpQueue q;
        for (int k = 0; k < 5; ++k) {
            CHECK(q.Add(dir + "/pair" + std::to_string(k) + "_A.rgba16f", px, 8));
            CHECK(q.AddTime(k, 1000 + k));
            q.pairs_queued = k + 1; if (k < 5) q.synth_left += 2;
            q.MaybeFlush(dir.c_str(), 5, false);
            CHECK(access((dir + "/pair0_A.rgba16f").c_str(), F_OK) != 0);        // not inside the window
        }
        q.synth_left = 0;
        q.MaybeFlush(dir.c_str(), 5, false);
        for (int k = 0; k < 5; ++k) CHECK(Slurp(dir + "/pair" + std::to_string(k) + "_A.rgba16f") == std::string((const char*)px, 8));
        CHECK(Slurp(dir + "/times.txt") == "pair0_A 1000\npair1_A 1001\npair2_A 1002\npair3_A 1003\npair4_A 1004\n");
        struct stat st; CHECK(stat((dir + "/times.txt").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(log().find("capture complete, 6 of 6 files written") != std::string::npos);
        CHECK(q.items.empty() && q.pairs_queued == 0);
    }
    {   // forced flush when the synthetic frames never came
        DumpQueue q; q.Add(dir + "/a.bin", px, 8); q.pairs_queued = 5; q.synth_left = 4;
        q.MaybeFlush(dir.c_str(), 5, false); CHECK(access((dir + "/a.bin").c_str(), F_OK) != 0);
        q.MaybeFlush(dir.c_str(), 5, true);  CHECK(access((dir + "/a.bin").c_str(), F_OK) == 0);
    }
    {   // a folder that cannot be written: FAILED, not "complete"
        const std::string ro = dir + "/ro"; mkdir(ro.c_str(), 0500);
        DumpQueue q; q.Add(ro + "/x.bin", px, 8); q.AddTime(0, 1); q.pairs_queued = 5;
        q.MaybeFlush(ro.c_str(), 5, false);
        const bool root = geteuid() == 0;
        if (!root) CHECK(log().find("capture FAILED, 0 of 2 files written") != std::string::npos);
        chmod(ro.c_str(), 0700);
    }
    {   // a symlink in place of a file is refused
        symlink("/nonexistent/target", (dir + "/link.bin").c_str());
        DumpQueue q; q.Add(dir + "/link.bin", px, 8); q.pairs_queued = 5; q.MaybeFlush(dir.c_str(), 5, false);
        CHECK(log().find("link.bin: FAILED to open") != std::string::npos);
    }
    {   // teardown inside the window
        DumpQueue q; q.Add(dir + "/p.bin", px, 8); q.Add(dir + "/q.bin", px, 8); q.AddTime(0, 1);
        q.Drop("the game ended");
        CHECK(log().find("2 queued images not written (the game ended)") != std::string::npos);
        CHECK(q.items.empty() && q.times.empty() && access((dir + "/p.bin").c_str(), F_OK) != 0);
        q.Drop("again");                                                       // nothing queued: no second line
        CHECK(log().find("again") == std::string::npos);
    }
    std::fclose(g_fg_log); g_fg_log = nullptr;
    std::system(("rm -rf " + dir).c_str());
    std::printf(g_fail ? "FAILED\n" : "dump queue: ok\n");
    return g_fail ? 1 : 0;
}
