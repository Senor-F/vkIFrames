// VKIFRAME_ONESIDED_FILL is experimental: off by default, ignored (with a log line) unless VKIFRAME_EXPERIMENTAL=1 is set too, read at game start from the
// environment or live.conf. One scenario per process (the configuration is read once): argv[1].
#include "live_config.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    const std::string sc = argc > 1 ? argv[1] : "default";
    char dir[] = "/tmp/vkifill_XXXXXX"; if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/live.conf", log = std::string(dir) + "/live.log";
    for (const char* v : {"VKIFRAME_ONESIDED_FILL", "VKIFRAME_EXPERIMENTAL", "FRAMEGEN_ONESIDED_FILL", "FRAMEGEN_EXPERIMENTAL"}) unsetenv(v);
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1); setenv("VKIFRAME_LIVE_LOG", log.c_str(), 1);
    auto write = [&](const std::string& t) { std::ofstream f(conf, std::ios::trunc); f << t; };
    bool want = false, ignored_logged = false;
    if (sc == "env_without_experimental") { setenv("VKIFRAME_ONESIDED_FILL", "1", 1); ignored_logged = true; }
    else if (sc == "env_with_experimental") { setenv("VKIFRAME_ONESIDED_FILL", "1", 1); setenv("VKIFRAME_EXPERIMENTAL", "1", 1); want = true; }
    else if (sc == "conf_with_experimental") { write("VKIFRAME_EXPERIMENTAL=1\nVKIFRAME_ONESIDED_FILL=1\n"); want = true; }
    else if (sc == "conf_without_experimental") { write("VKIFRAME_ONESIDED_FILL=1\n"); ignored_logged = true; }
    else if (sc == "conf_zero") { write("VKIFRAME_EXPERIMENTAL=1\nVKIFRAME_ONESIDED_FILL=0\n"); }
    else if (sc != "default") { std::printf("unknown scenario\n"); return 2; }
    CHECK(GetConfig().onesided_fill == want);
    std::fflush(nullptr);
    std::string text; { std::ifstream f(log); std::stringstream s; s << f.rdbuf(); text = s.str(); }
    CHECK((text.find("VKIFRAME_ONESIDED_FILL is experimental") != std::string::npos) == ignored_logged);
    std::string cmd = std::string("rm -rf ") + dir; if (std::system(cmd.c_str()) != 0) ++g_fail;
    std::printf(g_fail ? "fill gate %s: FAILED\n" : "fill gate %s: ok\n", sc.c_str());
    return g_fail ? 1 : 0;
}
