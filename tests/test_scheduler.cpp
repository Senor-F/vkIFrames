// Replays the traces written by tools/clock_sim/export_ref.py through the C++ TimingEngine + PairScheduler and compares
// kind, flush, tick count, tick times and t with the Python simulator. No Vulkan, no GPU.
#include "output_clock.hpp"
#include "timing_engine.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <dirent.h>

static const uint64_t kOffset = 700'000'000'000'000ull;       // ~8 days of uptime: checks that large clock values lose no precision

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data";
    DIR* d = opendir(dir.c_str());
    if (!d) { std::printf("FAIL: cannot open %s\n", dir.c_str()); return 1; }
    std::vector<std::string> files;
    while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.rfind("ref_", 0) == 0) files.push_back(dir + "/" + n); }
    closedir(d);
    if (files.empty()) { std::printf("FAIL: no ref_*.txt in %s\n", dir.c_str()); return 1; }

    int total_bad = 0; long total_lines = 0, total_ticks = 0;
    for (const auto& path : files) {
        std::ifstream in(path); std::string line;
        std::getline(in, line);                                   // "# target_fps kmax"
        double fps = 60; int kmax = 3; std::sscanf(line.c_str(), "# %lf %d", &fps, &kmax);
        PairScheduler::Params p; p.target_ns = 1e9 / fps; p.k_max = kmax; p.min_gain = 1.12;
        TimingEngine timing; PairScheduler sched;
        int bad = 0; long n_lines = 0, n_ticks = 0; double max_dt = 0, max_dtt = 0;
        while (std::getline(in, line)) {
            std::istringstream s(line);
            uint64_t now_py; char kind; int flush, n;
            s >> now_py >> kind >> flush >> n;
            std::vector<std::pair<double, double>> exp(n);
            for (auto& x : exp) s >> x.first >> x.second;
            const uint64_t now = now_py + kOffset;
            timing.Update(now);
            auto plan = sched.OnNativeFrame(double(now), timing.SmoothedExactNs(), timing.JitterExactNs(), double(timing.LastRawNs()), p);
            ++n_lines;
            bool ok = true;
            switch (kind) {
                case 'B': ok = plan.kind == PairScheduler::Kind::Bypass && plan.flush_held == bool(flush); break;
                case 'H': ok = plan.kind == PairScheduler::Kind::Hold; break;
                case 'P': ok = plan.kind == PairScheduler::Kind::Pair && !plan.carrier_only && plan.n == n; break;
                case 'C': ok = plan.kind == PairScheduler::Kind::Pair && plan.carrier_only && n == 1; break;
            }
            if (ok && kind == 'P') {
                for (int i = 0; i < n; ++i) {
                    const double dt = std::abs((plan.ticks[i].time_ns - double(kOffset)) - exp[i].first), dtt = std::abs(plan.ticks[i].t - exp[i].second);
                    max_dt = std::max(max_dt, dt); max_dtt = std::max(max_dtt, dtt);
                    if (dt > 50.0 || dtt > 1e-4) ok = false;       // 50 ns, 1e-4 of a frame
                    ++n_ticks;
                }
            }
            if (ok && kind == 'C') { const double dt = std::abs((plan.carrier_time_ns - double(kOffset)) - exp[0].first); max_dt = std::max(max_dt, dt); if (dt > 50.0) ok = false; ++n_ticks; }
            if (!ok && ++bad <= 3) std::printf("  mismatch %s line %ld (kind %c, now %llu)\n", path.c_str(), n_lines + 1, kind, (unsigned long long)now_py);
        }
        std::printf("%-40s %4ld frames %5ld ticks  max |dtick| %.3f ns  max |dt| %.2e  %s\n", path.c_str(), n_lines, n_ticks, max_dt, max_dtt, bad ? "MISMATCH" : "ok");
        total_bad += bad; total_lines += n_lines; total_ticks += n_ticks;
    }
    // Invariants on their own (not from Python): a held frame is flushed when interpolation stops.
    {
        TimingEngine t; PairScheduler s; PairScheduler::Params p; uint64_t now = kOffset; bool saw_flush = false, saw_pair = false;
        for (int i = 0; i < 200; ++i) { now += 25'000'000; t.Update(now); auto pl = s.OnNativeFrame(double(now), t.SmoothedExactNs(), t.JitterExactNs(), double(t.LastRawNs()), p); saw_pair |= pl.kind == PairScheduler::Kind::Pair; }
        now += 250'000'000; t.Update(now);                        // a 250 ms stall
        auto pl = s.OnNativeFrame(double(now), t.SmoothedExactNs(), t.JitterExactNs(), double(t.LastRawNs()), p);
        saw_flush = pl.kind == PairScheduler::Kind::Bypass && pl.flush_held && !s.Holding();
        if (!saw_pair || !saw_flush) { std::printf("FAIL: stall did not flush the held frame (pair %d flush %d)\n", saw_pair, saw_flush); ++total_bad; }
        else std::printf("stall flush ok\n");
    }
    // Phase lock: 45 fps (with jitter) -> 90 Hz; after settling, one tick per pair must land on the native frame.
    for (int lock = 0; lock < 2; ++lock) {
        TimingEngine t; PairScheduler s; PairScheduler::Params p; p.target_ns = 1e9 / 90.0; p.phase_lock = lock;
        uint64_t now = kOffset; int pairs = 0, aligned = 0; unsigned rng = 12345;
        for (int i = 0; i < 1500; ++i) {
            rng = rng * 1664525u + 1013904223u;
            now += 22'222'222 + (long)((rng >> 8) % 600000) - 300000;
            t.Update(now);
            auto pl = s.OnNativeFrame(double(now), t.SmoothedExactNs(), t.JitterExactNs(), double(t.LastRawNs()), p);
            if (pl.kind != PairScheduler::Kind::Pair || i < 1000) continue;
            ++pairs; bool a = false; for (int k = 0; k < pl.n; ++k) a |= pl.ticks[k].native_a || pl.ticks[k].t > 0.98;
            aligned += a;
        }
        std::printf("phase lock %d: %d/%d pairs aligned\n", lock, aligned, pairs);
        if (lock && aligned < pairs * 8 / 10) { std::printf("FAIL: phase lock did not align\n"); ++total_bad; }
    }
    // Phase offset 0.5: the locked ticks sit between the native frames (t = 0.25 and 0.75 at a ratio of 2)
    {
        TimingEngine t; PairScheduler s; PairScheduler::Params p; p.target_ns = 1e9 / 90.0; p.phase_lock = true; p.phase_offset = 0.5;
        uint64_t now = kOffset; int pairs = 0, ok = 0; unsigned rng = 777;
        for (int i = 0; i < 1500; ++i) {
            rng = rng * 1664525u + 1013904223u;
            now += 22'222'222 + (long)((rng >> 8) % 600000) - 300000;
            t.Update(now);
            auto pl = s.OnNativeFrame(double(now), t.SmoothedExactNs(), t.JitterExactNs(), double(t.LastRawNs()), p);
            if (pl.kind != PairScheduler::Kind::Pair || i < 1000 || pl.n != 2) continue;
            ++pairs; ok += std::fabs(pl.ticks[0].t - 0.25) < 0.08 && std::fabs(pl.ticks[1].t - 0.75) < 0.08;
        }
        std::printf("phase offset 0.5: %d/%d pairs at t = 0.25 / 0.75\n", ok, pairs);
        if (pairs < 300 || ok < pairs * 8 / 10) { std::printf("FAIL: phase offset\n"); ++total_bad; }
    }
    // Extrapolation: tick 0 is the new frame on a slot of the fixed clock, then one predicted tick per free slot before the next frame is due: all slots
    // are used (output = target), the ticks of one job end before the next frame's slot, and t stays inside (0, 1).
    for (double base_fps : {45.0, 15.0, 30.0, 40.0, 60.0}) {
        TimingEngine t; PairScheduler s; PairScheduler::Params p; p.target_ns = 1e9 / 90.0; p.k_max = 5;
        const uint64_t step = uint64_t(1e9 / base_fps); uint64_t now = kOffset; int pairs = 0, good_n = 0, bad_t = 0, overlap = 0; long ticks = 0;
        double prev_end = 0;
        for (int i = 0; i < 600; ++i) {
            now += step; t.Update(now);
            auto pl = s.OnNativeFrameExtrap(double(now), t.SmoothedExactNs(), double(t.LastRawNs()), p);
            if (pl.kind != PairScheduler::Kind::Pair || i < 100) continue;
            ++pairs; ticks += pl.n;
            if (pl.ticks[0].time_ns <= prev_end) ++overlap;                 // B must come after the last tick of the job before
            for (int k = 1; k < pl.n; ++k) if (!(pl.ticks[k].t > 0.0 && pl.ticks[k].t < 1.0)) ++bad_t;
            for (int k = 1; k < pl.n; ++k) if (std::fabs(pl.ticks[k].time_ns - pl.ticks[0].time_ns - k * p.target_ns) > 1.0) ++bad_t;
            prev_end = pl.ticks[pl.n - 1].time_ns;
        }
        const double per_native = double(ticks) / double(pairs), want = 90.0 / base_fps;
        std::printf("extrapolation %.0f -> 90: %.3f ticks per native frame (want %.3f), overlaps %d, bad t %d\n", base_fps, per_native, want, overlap, bad_t);
        good_n = std::fabs(per_native - want) < 0.03 * want;
        if (pairs < 400 || !good_n || overlap || bad_t) { std::printf("FAIL: extrapolation plan\n"); ++total_bad; }
    }
    if (total_bad) { std::printf("FAIL: %d mismatching frames\n", total_bad); return 1; }
    std::printf("PASS: scheduler matches clock_sim on %zu traces (%ld frames, %ld ticks)\n", files.size(), total_lines, total_ticks);
}
