// Hostile or broken VKIFRAME_* values in the environment: a number that is nan, inf, hex, empty, out of range or followed by text must never reach the layer as a
// number. It keeps its default (the setting is ignored, and the layer says so); a finite number is clamped to the setting's range. No GPU needed.
// usage: test_hostile_env a|b   (two processes: the layer reads its environment once)
#include "clean_env.hpp"
#include "live_config.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "a";
    ClearFramegenEnv();
    char dir[] = "/tmp/vkihostile_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string log = std::string(dir) + "/layer.log";
    setenv("VKIFRAME_LOG_FILE", log.c_str(), 1);               // the layer's own lines go here, not into a real ~/.config
    setenv("XDG_CONFIG_HOME", dir, 1);
    const Config d{};                                          // the built-in defaults
    if (mode == "a") {
        setenv("VKIFRAME_SYNTH_MARGIN", "nan", 1);
        setenv("VKIFRAME_PHASE_OFFSET", "inf", 1);
        setenv("VKIFRAME_MIN_GAIN", "0x10", 1);
        setenv("VKIFRAME_UI_MASK_THRESHOLD", "-nan", 1);
        setenv("VKIFRAME_CONF_LO", "nan", 1);
        setenv("VKIFRAME_CONF_HI", "1e999", 1);
        setenv("VKIFRAME_PAN_LO", "", 1);
        setenv("VKIFRAME_PAN_HI", "0.5abc", 1);
        setenv("VKIFRAME_INTERP_GUIDE", "0x1p3", 1);
        setenv("VKIFRAME_INTERP_CAND", "infinity", 1);
        setenv("VKIFRAME_TARGET_FPS", "1e30", 1);              // a finite number: clamped, not ignored
        setenv("VKIFRAME_BASE_FPS_CAP", "0.0001", 1);          // below 1 fps: off
        const Config& c = GetConfig();
        CHECK(c.synth_margin == d.synth_margin && c.phase_offset == d.phase_offset && c.min_gain == d.min_gain && c.ui_mask == d.ui_mask);
        CHECK(c.conf_lo == d.conf_lo && c.conf_hi == d.conf_hi && c.pan_lo == d.pan_lo && c.pan_hi == d.pan_hi);
        CHECK(c.interp_guide == d.interp_guide && c.cand_thr == d.cand_thr);
        CHECK(c.target_fps == 1000.0f);
        CHECK(c.base_fps_cap == 0.0f);
        CHECK(Live().base_fps_cap.load() == 0.0f && Live().target_fps.load() == 1000.0f);
    } else {
        setenv("VKIFRAME_TARGET_FPS", "-5", 1);
        setenv("VKIFRAME_BASE_FPS_CAP", "1e30", 1);
        setenv("VKIFRAME_SYNTH_MARGIN", "0.9", 1);             // finite: clamped to the range
        setenv("VKIFRAME_MIN_GAIN", "  1.5  ", 1);             // blanks around a number are fine
        setenv("VKIFRAME_PAN_HI", "1e-1", 1);
        const Config& c = GetConfig();
        CHECK(c.target_fps == 1.0f);
        CHECK(c.base_fps_cap == 90.0f);
        CHECK(c.synth_margin == 0.45f && c.min_gain == 1.5f && c.pan_hi == 0.1f);
    }
    std::fflush(stderr);
    std::ifstream f(log); std::stringstream ss; ss << f.rdbuf();
    if (mode == "a") {
        CHECK(ss.str().find("VKIFRAME_SYNTH_MARGIN=\"nan\" is not a usable number") != std::string::npos);
        CHECK(ss.str().find("VKIFRAME_MIN_GAIN=\"0x10\" is not a usable number") != std::string::npos);
    }
    std::printf(g_fail ? "hostile env %s: FAILED\n" : "hostile env %s: ok\n", mode.c_str());
    std::string cmd = std::string("rm -rf ") + dir; if (std::system(cmd.c_str()) != 0) {}
    return g_fail ? 1 : 0;
}
