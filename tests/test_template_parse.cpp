// The layer's own parser on manifest/live.conf.template: a file of comment lines only. It must log one summary line (no setting line, nothing invalid or unknown) and
// no note, and change no value. usage: test_template_parse <template path>
#include "clean_env.hpp"
#include "live_config.hpp"
#include "vk_iframes_layer.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static std::string Slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int, char** argv) {
    ClearFramegenEnv();
    char dir[] = "/tmp/vkitplparse_XXXXXX"; if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/live.conf", log = std::string(dir) + "/live.log";
    { std::ofstream o(conf, std::ios::binary); o << Slurp(argv[1]); }
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1);
    setenv("VKIFRAME_LIVE_LOG", log.c_str(), 1);
    setenv("VKIFRAME_TARGET_FPS", "75", 1);                      // a launch-option value: the template must not override it
    setenv("VKIFRAME_BASE_FPS_CAP", "30", 1);
    LiveSettings& L = Live();
    LivePoll(1'000'000'000ull);
    const std::string text = Slurp(log);
    CHECK(L.target_fps == 75.0f && L.base_fps_cap == 30.0f && L.enabled == 1);          // nothing from the file
    CHECK(text.find("0 setting line(s)") != std::string::npos);
    CHECK(text.find("0 invalid, 0 unknown") != std::string::npos);
    CHECK(text.find("line ") == std::string::npos && text.find("ignored") == std::string::npos && text.find("keeping") == std::string::npos);   // no note
    size_t lines = 0; for (char c : text) if (c == '\n') ++lines;
    CHECK(lines <= 3);                                                                    // the session start line and the one summary line
    std::printf(g_fail ? "template parse: FAILED\n%s\n" : "template parse: ok\n", g_fail ? text.c_str() : "");
    return g_fail ? 1 : 0;
}
