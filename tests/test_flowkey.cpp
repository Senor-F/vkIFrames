// The optical-flow setting was renamed twice (WARP -> INTERPOLATE -> OPTICAL_FLOW). Every spelling still works, in the environment and in live.conf, the newest
// name wins when more than one is given, a live.conf value wins over the environment (a start-of-game key), and with nothing set the default is off (opt-in).
// One scenario per process (GetConfig() and the start-of-game values are read once): usage: test_flowkey <scenario>
#include "clean_env.hpp"
#include "vk_iframes_layer.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    const std::string sc = argc > 1 ? argv[1] : "";
    ClearFramegenEnv();
    char dir[] = "/tmp/vkiflowkey_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/live.conf", log = std::string(dir) + "/layer.log";
    setenv("VKIFRAME_LOG_FILE", log.c_str(), 1);
    setenv("VKIFRAME_LIVE_LOG", (std::string(dir) + "/live.log").c_str(), 1);
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1);
    auto file = [&](const char* text) { std::ofstream f(conf, std::ios::binary); f << text; };
    bool want = false;
    if (sc == "default")              { want = false; }                                                                // nothing set
    else if (sc == "env_new")         { setenv("VKIFRAME_OPTICAL_FLOW", "1", 1); want = true; }
    else if (sc == "env_interpolate") { setenv("VKIFRAME_INTERPOLATE", "1", 1); want = true; }                         // an alpha.1 launch option
    else if (sc == "env_warp")        { setenv("VKIFRAME_WARP", "1", 1); want = true; }
    else if (sc == "env_framegen")    { setenv("FRAMEGEN_INTERPOLATE", "1", 1); want = true; }                         // the oldest prefix
    else if (sc == "env_new_wins")    { setenv("VKIFRAME_INTERPOLATE", "1", 1); setenv("VKIFRAME_OPTICAL_FLOW", "0", 1); want = false; }
    else if (sc == "env_interp_over_warp") { setenv("VKIFRAME_WARP", "1", 1); setenv("VKIFRAME_INTERPOLATE", "0", 1); want = false; }
    else if (sc == "file_new")        { file("REV=1\nVKIFRAME_OPTICAL_FLOW=1\n"); want = true; }
    else if (sc == "file_interpolate"){ file("REV=1\nVKIFRAME_INTERPOLATE=1\n"); want = true; }                         // written by the alpha.1 plugin
    else if (sc == "file_warp")       { file("REV=1\nWARP=1\n"); want = true; }
    else if (sc == "file_new_wins")   { file("REV=1\nVKIFRAME_OPTICAL_FLOW=0\nVKIFRAME_INTERPOLATE=1\nWARP=1\n"); want = false; }
    else if (sc == "file_new_wins_rev") { file("REV=1\nWARP=1\nVKIFRAME_INTERPOLATE=1\nVKIFRAME_OPTICAL_FLOW=0\n"); want = false; }
    else if (sc == "file_interp_over_warp") { file("REV=1\nINTERPOLATE=0\nWARP=1\n"); want = false; }
    else if (sc == "file_over_env")   { file("REV=1\nVKIFRAME_OPTICAL_FLOW=0\n"); setenv("VKIFRAME_OPTICAL_FLOW", "1", 1); want = false; }
    else if (sc == "file_old_over_env") { file("REV=1\nVKIFRAME_INTERPOLATE=1\n"); setenv("VKIFRAME_OPTICAL_FLOW", "0", 1); want = true; }   // the file wins over the environment, whatever the spelling
    else { std::printf("unknown scenario\n"); return 2; }
    const bool got = GetConfig().interpolate;
    const bool flow_built_for = GetConfig().flow;                 // implied by the setting
    int bad = 0;
    if (got != want) { std::printf("FAIL %s: interpolate=%d, wanted %d\n", sc.c_str(), int(got), int(want)); ++bad; }
    if (flow_built_for != want) { std::printf("FAIL %s: flow=%d, wanted %d\n", sc.c_str(), int(flow_built_for), int(want)); ++bad; }
    std::printf(bad ? "flowkey %s: FAILED\n" : "flowkey %s: ok\n", sc.c_str());
    return bad ? 1 : 0;
}
