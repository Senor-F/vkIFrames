// The layer's own parser on manifest/live.conf.template: a file of comment lines only. It must log one summary line (no setting line, nothing invalid or unknown) and
// no note, and change no value. With "uncommented" every setting line has its "# " removed (what the header tells the user to do): then all of them must be read as
// valid, with the default values. usage: test_template_parse <template path> [uncommented]
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

int main(int argc, char** argv) {
    ClearFramegenEnv();
    char dir[] = "/tmp/vkitplparse_XXXXXX"; if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/live.conf", log = std::string(dir) + "/live.log";
    const bool unc = argc > 2 && std::string(argv[2]) == "uncommented";
    {
        std::ofstream o(conf, std::ios::binary);
        std::stringstream in(Slurp(argv[1])); std::string line;
        while (std::getline(in, line)) {
            bool setting = false;                                     // "# KEY=" at the start of the line (KEY is capitals and underscores)
            if (unc && line.size() > 4 && line[0] == '#' && line[1] == ' ') {
                size_t i = 2; while (i < line.size() && ((line[i] >= 'A' && line[i] <= 'Z') || line[i] == '_')) ++i;
                setting = i > 2 && i < line.size() && line[i] == '=';
            }
            o << (setting ? line.substr(2) : line) << "\n";
        }
    }
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1);
    setenv("VKIFRAME_LIVE_LOG", log.c_str(), 1);
    if (!unc) { setenv("VKIFRAME_TARGET_FPS", "75", 1); setenv("VKIFRAME_BASE_FPS_CAP", "30", 1); }   // launch-option values: the commented template must not override them
    LiveSettings& L = Live();
    LivePoll(1'000'000'000ull);
    const std::string text = Slurp(log);
    if (!unc) {
        CHECK(L.target_fps == 75.0f && L.base_fps_cap == 30.0f && L.enabled == 1);          // nothing from the file
        CHECK(text.find("0 setting line(s)") != std::string::npos);
    } else {
        CHECK(text.find("14 setting line(s)") != std::string::npos && text.find("4 start-of-game only") != std::string::npos);
        CHECK(L.enabled == 1 && L.target_fps == 60.0f && L.max_synthetic == 3 && L.base_fps_cap == 45.0f && L.min_gain == 1.01f && L.phase_lock == 1);
        CHECK(L.phase_offset == 0.5f && L.interp_bidir == 1 && L.cmd_buffers == 8 && L.synth_margin == 0.15f);
        const Config& c = GetConfig();                                                       // the start-of-game lines, read by the same code the layer uses
        CHECK(!c.interpolate && !c.extrapolate && c.ui_mask == 0.08f && c.flow_level == 2);
    }
    CHECK(text.find("0 invalid, 0 unknown") != std::string::npos);
    CHECK(text.find("line ") == std::string::npos && text.find("ignored") == std::string::npos && text.find("keeping") == std::string::npos && text.find("not a usable") == std::string::npos && text.find("not a valid") == std::string::npos);   // no note
    size_t lines = 0; for (char c : text) if (c == '\n') ++lines;
    CHECK(lines <= (unc ? 8u : 3u));                                                                    // the session start line and the one summary line
    std::printf(g_fail ? "template parse: FAILED\n%s\n" : "template parse: ok\n", g_fail ? text.c_str() : "");
    return g_fail ? 1 : 0;
}
