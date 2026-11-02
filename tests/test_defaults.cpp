// Defaults: the Decky panel's schema against the layer (every default and range, in both directions), the safety defaults of the layer (nothing that
// writes files, costs GPU time or is experimental is on without being asked) and the manifest's opt-in (VKIFRAME_ENABLE=1).
#include "live_config.hpp"
#include "schema_table.h"
#include "vk_iframes_layer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
#define FAIL(...) do { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++g_fail; } while (0)

// Known, documented differences that the owner has decided to keep: none. A schema key whose DEFAULT differs from the layer's fails the test.
static bool Known(const char*) { return false; }

int main() {
    const Config d{};                                        // the layer's built-in defaults, before any environment variable
    LiveKeyRow rows[32]; const int n = LiveKeyTable(rows, 32);
    auto layer = [&](const char* key) -> const LiveKeyRow* { for (int i = 0; i < n; ++i) if (!std::strcmp(rows[i].name, key)) return &rows[i]; return nullptr; };

    // start-of-game keys: the layer's default comes from Config
    auto start_default = [&](const char* key, double& out) {
        if (!std::strcmp(key, "OPTICAL_FLOW")) { out = d.interpolate ? 1 : 0; return true; }
        if (!std::strcmp(key, "EXTRAPOLATE")) { out = d.extrapolate ? 1 : 0; return true; }
        if (!std::strcmp(key, "UI_MASK_THRESHOLD")) { out = double(d.ui_mask); return true; }
        if (!std::strcmp(key, "FLOW_LEVEL")) { out = d.flow_level; return true; }
        if (!std::strcmp(key, "ONESIDED_FILL")) { out = d.onesided_fill ? 1 : 0; return true; }
        if (!std::strcmp(key, "EXPERIMENTAL")) { out = 0; return true; }       // read from the environment / live.conf only; off unless set
        return false;
    };
    for (const SchemaRow& s : kSchema) {
        double ld = 0;
        bool have = false, check_range = false;
        double llo = 0, lhi = 0;
        if (s.live) {
            const LiveKeyRow* r = layer(s.key);
            if (!r) { FAIL("schema key %s is live but the layer has no such live key", s.key); continue; }
            ld = r->def; llo = r->lo; lhi = r->hi; have = true; check_range = !s.toggle;
            if (s.toggle != r->toggle) FAIL("%s: toggle in the schema = %d, in the layer = %d", s.key, int(s.toggle), int(r->toggle));
        } else if (start_default(s.key, ld)) {
            have = true;
        } else {
            FAIL("schema key %s is unknown to this test (add it to the layer's table or to start_default)", s.key);
            continue;
        }
        if (have && std::fabs(s.def - ld) > 1e-6 && !Known(s.key)) FAIL("%s: default in the schema = %g, in the layer = %g", s.key, s.def, ld);
        if (have && std::fabs(s.def - ld) <= 1e-6 && Known(s.key)) FAIL("%s: listed as a known difference but the defaults agree now: remove it from the list", s.key);
        if (check_range && (s.lo < llo - 1e-9 || s.hi > lhi + 1e-9)) FAIL("%s: the schema range %g..%g is not inside the layer's %g..%g (the layer would clamp what the panel sends)", s.key, s.lo, s.hi, llo, lhi);
        if (check_range && (!std::strcmp(s.key, "TARGET_FPS") || !std::strcmp(s.key, "BASE_FPS_CAP")) && (std::fabs(s.lo - llo) > 1e-9 || std::fabs(s.hi - lhi) > 1e-9))
            FAIL("%s: the schema range %g..%g and the layer's %g..%g must be the same (the owner raised these limits)", s.key, s.lo, s.hi, llo, lhi);
        if (check_range && (s.def < s.lo - 1e-9 || s.def > s.hi + 1e-9)) FAIL("%s: the schema's default %g is outside its own range", s.key, s.def);
    }
    // the table by machine: the layer's (MachineDefaultsFor) against the schema's "machine_defaults" for the two keys that have one, and nothing else may differ by machine
    {
        const char* path = "/tmp/vkiframes_test_defaults_dmi";
        auto board = [&](const char* name) { { std::ofstream o(path, std::ios::trunc); o << name << "\n"; } return MachineDefaultsFor(path); };
        const MachineDefaults gal = board("Galileo"), jup = board("Jupiter"), other = board("MS-7C94");
        for (const SchemaRow& s : kSchema) {
            const bool cap = !std::strcmp(s.key, "BASE_FPS_CAP"), tgt = !std::strcmp(s.key, "TARGET_FPS");
            if (cap) { CHECK(s.galileo == gal.base_fps_cap && s.jupiter == jup.base_fps_cap && s.def == other.base_fps_cap); }
            else if (tgt) { CHECK(s.galileo == gal.target_fps && s.jupiter == jup.target_fps && s.def == other.target_fps); }
            else CHECK(s.galileo == s.def && s.jupiter == s.def);          // no other setting has a machine default
        }
        CHECK(gal.base_fps_cap == 45.0f && gal.target_fps == 90.0f && jup.base_fps_cap == 30.0f && jup.target_fps == 60.0f && other.base_fps_cap == 60.0f && other.target_fps == 120.0f);
        std::remove(path);
    }
    // the other direction: every live key of the layer is in the schema (STATS is a measurement switch the panel does not show)
    for (int i = 0; i < n; ++i) {
        if (!std::strcmp(rows[i].name, "STATS")) continue;
        bool found = false;
        for (const SchemaRow& s : kSchema) if (s.live && !std::strcmp(s.key, rows[i].name)) found = true;
        if (!found) FAIL("the layer's live key %s is missing in the schema", rows[i].name);
    }

    // the layer's safety defaults: nothing experimental, nothing that writes files or costs GPU time unasked
    CHECK(d.dump_pair == 0);                                  // VKIFRAME_DUMP: off
    CHECK(!d.debug_flow && !d.debug_ring && d.tickmark == 0);
    CHECK(!d.onesided_fill);                                  // the experimental fill is off
    CHECK(!d.extrapolate && !d.pace_sleep && !d.absolute && !d.cap_early && d.cand_thr == 0.0f);
    CHECK(DumpNeedsFlowNote(1, d.flow) && std::string(DumpNeedsFlowNote(1, d.flow)) == "VKIFRAME_DUMP needs VKIFRAME_OPTICAL_FLOW=1; nothing will be captured");   // the capture needs the flow: said once in the log
    CHECK(!DumpNeedsFlowNote(0, d.flow) && !DumpNeedsFlowNote(1, true));
    CHECK(!d.interpolate && !d.flow);                         // optical flow is opt-in: by default the synthetic frames are a plain cross-fade and no flow engine is built
    CHECK(d.hold && d.synth && d.presenter);                  // the product runs by default once enabled
    CHECK(d.max_synthetic <= kLiveMaxSyntheticNormal);
    CHECK(LiveMaxSyntheticLimit() == kLiveMaxSyntheticNormal);   // EXPERIMENTAL is off: the limit is 4, not 10
    CHECK(Live().stats.load() == 0 && Live().enabled.load() == 1);
    CHECK(d.dump_frames == 5);

    // the manifest turns the layer on only for a process that sets VKIFRAME_ENABLE=1, and VKIFRAME_DISABLE=1 wins
    std::ifstream f(FRAMEGEN_MANIFEST_IN); std::stringstream ss; ss << f.rdbuf(); const std::string m = ss.str();
    CHECK(!m.empty());
    CHECK(m.find("\"enable_environment\": { \"VKIFRAME_ENABLE\": \"1\" }") != std::string::npos);
    CHECK(m.find("\"disable_environment\": { \"VKIFRAME_DISABLE\": \"1\" }") != std::string::npos);

    std::printf(g_fail ? "defaults: FAILED\n" : "defaults: ok\n");
    return g_fail ? 1 : 0;
}
