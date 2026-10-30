#include "stats.hpp"

#include "live_config.hpp"
#include "vk_iframes_layer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace stats {

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    size_t rank = size_t(std::ceil(p * double(n)));       // nearest rank, counting from 1
    rank = std::clamp<size_t>(rank, 1, n);
    return v[rank - 1];
}

double FrameTimes::Mean() const {
    if (ms.empty()) return 0.0;
    double s = 0.0;
    for (double v : ms) s += v;
    return s / double(ms.size());
}
double FrameTimes::Max() const { return ms.empty() ? 0.0 : *std::max_element(ms.begin(), ms.end()); }

void FillDerived(Record& r, const FrameTimes& ft) {
    r.frames = ft.Count();
    r.ft_mean = ft.Mean(); r.ft_p95 = Percentile(ft.ms, 0.95); r.ft_p99 = Percentile(ft.ms, 0.99); r.ft_max = ft.Max();
    r.gpu_share = (r.pairs_per_s * r.flow_ms + r.generated_per_s * r.interp_ms) / 1000.0;
}

std::string RecordJson(const Record& r, long long unix_s) {
    char b[1400];
    std::snprintf(b, sizeof b,
        "{\"t\":%lld,\"secs\":%.3f,\"mode\":\"%s\",\"bench\":\"%s\",\"target_fps\":%.1f,\"refresh_hz\":%.2f,\"native_fps\":%.2f,\"shown_fps\":%.2f,\"gen_per_native\":%.3f,"
        "\"frames\":%llu,\"ft_mean_ms\":%.3f,\"ft_p95_ms\":%.3f,\"ft_p99_ms\":%.3f,\"ft_max_ms\":%.3f,"
        "\"cpu_game_ms\":%.4f,\"presenter_core\":%.4f,"
        "\"wait_lock_ms\":%.4f,\"wait_ring_ms\":%.4f,\"wait_fence_ms\":%.4f,\"wait_acquire_ms\":%.4f,\"wait_present_ms\":%.4f,"
        "\"flow_ms\":%.4f,\"pairs_per_s\":%.2f,\"interp_ms\":%.4f,\"generated_per_s\":%.2f,\"gpu_share\":%.5f,"
        "\"late_mean_ms\":%.3f,\"late_max_ms\":%.3f,\"cancelled\":%llu,\"dropped\":%llu}",
        unix_s, r.secs, r.mode.c_str(), r.bench.c_str(), r.target_fps, r.refresh_hz, r.native_fps, r.shown_fps, r.gen_per_native,
        (unsigned long long)r.frames, r.ft_mean, r.ft_p95, r.ft_p99, r.ft_max,
        r.cpu_game_ms, r.presenter_core,
        r.wait_lock_ms, r.wait_ring_ms, r.wait_fence_ms, r.wait_acquire_ms, r.wait_present_ms,
        r.flow_ms, r.pairs_per_s, r.interp_ms, r.generated_per_s, r.gpu_share,
        r.late_mean_ms, r.late_max_ms, (unsigned long long)r.cancelled, (unsigned long long)r.dropped);
    return b;
}

// ---------------------------------------------------------------- benchmark

void Bench::Start(int user_enabled, double target_fps, int max_synthetic, double base_fps_cap, double min_gain, int bidir) {
    state_ = State::Running; reason_.clear(); elapsed_ = 0.0; rows_.clear();
    user_enabled_ = user_enabled; cur_enabled_ = 1;
    target_ = target_fps; synth_ = max_synthetic; cap_ = base_fps_cap; gain_ = min_gain; bidir_ = bidir; refresh_ = 0.0;
}

int Bench::SegmentOf(double t, bool& on, double& into) const {
    if (t < p_.warmup_s) { on = true; into = t; return -1; }
    const double u = t - p_.warmup_s;
    int seg = int(std::floor(u / p_.segment_s));
    seg = std::clamp(seg, 0, 2 * p_.reps - 1);
    on = (seg % 2) == 1;                                  // segments alternate off, on, off, on ...
    into = u - double(seg) * p_.segment_s;
    return seg;
}

const char* Bench::Label() const {
    if (state_ != State::Running) return "";
    bool on; double into;
    const int seg = SegmentOf(elapsed_, on, into);
    if (seg < 0) return "warmup";
    if (into < p_.settle_s) return "settle";
    return on ? "on" : "off";
}

int Bench::OnSecond(Record& r) {
    if (state_ != State::Running) return user_enabled_;
    const std::string label = Label();
    r.bench = label;
    if (label == "off" || label == "on") {
        rows_.push_back({r, label == "on"});
        if (label == "on" && refresh_ == 0.0) refresh_ = r.refresh_hz;
    }
    elapsed_ += r.secs;
    if (elapsed_ >= p_.warmup_s + 2.0 * p_.reps * p_.segment_s - 1e-6) { state_ = State::Complete; return user_enabled_; }
    bool on; double into;
    SegmentOf(elapsed_, on, into);
    cur_enabled_ = on ? 1 : 0;
    return cur_enabled_;
}

void Bench::Abort(const char* reason) {
    if (state_ != State::Running) return;
    state_ = State::Aborted; reason_ = reason ? reason : "";
}

Bench::Agg Bench::Aggregate(bool on) const {
    Agg a;
    double frames = 0, pairs = 0, gen = 0;
    for (const Row& w : rows_) {
        if (w.on != on) continue;
        const Record& r = w.r;
        ++a.seconds;
        const double f = double(r.frames);
        frames += f; a.frames += r.frames;
        a.ft_mean += r.ft_mean * f; a.ft_p95 += r.ft_p95; a.ft_p99 += r.ft_p99; a.ft_max = std::max(a.ft_max, r.ft_max);
        a.native_fps += r.native_fps; a.shown_fps += r.shown_fps;
        a.cpu_game_ms += r.cpu_game_ms * f; a.presenter_core += r.presenter_core;
        a.wait_game_ms += (r.wait_acquire_ms + r.wait_lock_ms + r.wait_ring_ms + r.wait_fence_ms) * f;
        a.gpu_share += r.gpu_share;
        a.gpu_flow_share += r.pairs_per_s * r.flow_ms / 1000.0;
        a.gpu_interp_share += r.generated_per_s * r.interp_ms / 1000.0;
        a.flow_ms += r.flow_ms * r.pairs_per_s * r.secs; pairs += r.pairs_per_s * r.secs;
        a.interp_ms += r.interp_ms * r.generated_per_s * r.secs; gen += r.generated_per_s * r.secs;
    }
    if (a.seconds > 0) {
        const double n = double(a.seconds);
        if (frames > 0) { a.ft_mean /= frames; a.cpu_game_ms /= frames; a.wait_game_ms /= frames; }
        a.ft_p95 /= n; a.ft_p99 /= n; a.native_fps /= n; a.shown_fps /= n; a.presenter_core /= n;
        a.gpu_share /= n; a.gpu_flow_share /= n; a.gpu_interp_share /= n;
        a.flow_ms = pairs > 0 ? a.flow_ms / pairs : 0.0;
        a.interp_ms = gen > 0 ? a.interp_ms / gen : 0.0;
    }
    return a;
}

std::string Bench::Json() const {
    const Agg off = Aggregate(false), on = Aggregate(true);
    const bool ok = state_ == State::Complete && off.seconds > 0 && on.seconds > 0;
    auto agg = [](const Agg& a) {
        char b[900];
        std::snprintf(b, sizeof b,
            "{\"seconds\":%d,\"frames\":%llu,\"frame_time_mean_ms\":%.3f,\"frame_time_p95_ms\":%.3f,\"frame_time_p99_ms\":%.3f,\"frame_time_max_ms\":%.3f,"
            "\"native_fps\":%.2f,\"shown_fps\":%.2f,\"game_cpu_ms_per_frame\":%.4f,\"presenter_core_share\":%.4f,\"game_wait_ms_per_frame\":%.4f,"
            "\"gpu_share\":%.5f,\"gpu_flow_share\":%.5f,\"gpu_interp_share\":%.5f,\"flow_ms_per_pair\":%.4f,\"interp_ms_per_generated\":%.4f}",
            a.seconds, (unsigned long long)a.frames, a.ft_mean, a.ft_p95, a.ft_p99, a.ft_max, a.native_fps, a.shown_fps, a.cpu_game_ms, a.presenter_core, a.wait_game_ms,
            a.gpu_share, a.gpu_flow_share, a.gpu_interp_share, a.flow_ms, a.interp_ms);
        return std::string(b);
    };
    const char* status = state_ == State::Complete ? "complete" : state_ == State::Aborted ? "aborted" : "idle";
    std::string j;
    char b[700];
    std::snprintf(b, sizeof b,
        "{\n\"version\":1,\n\"status\":\"%s\",\n\"reason\":\"%s\",\n"
        "\"schedule\":{\"warmup_s\":%.0f,\"segment_s\":%.0f,\"repeats\":%d,\"settle_s_left_out_per_segment\":%.0f,\"order\":\"warm-up (layer on), then repeats x (off, on)\"},\n"
        "\"settings\":{\"target_fps\":%.1f,\"max_synthetic\":%d,\"base_fps_cap\":%.1f,\"min_gain\":%.3f,\"interp_bidir\":%d,\"refresh_hz\":%.2f,\"base_fps_cap_kept_while_off\":true},\n",
        status, reason_.c_str(), p_.warmup_s, p_.segment_s, p_.reps, p_.settle_s, target_, synth_, cap_, gain_, bidir_, refresh_);
    j += b;
    j += "\"off\":" + agg(off) + ",\n\"on\":" + agg(on) + ",\n\"derived\":";
    if (!ok) {
        j += "null\n}\n";
        return j;
    }
    const double cost = on.ft_mean - off.ft_mean;
    const double gpu = on.gpu_share;
    std::snprintf(b, sizeof b,
        "{\"layer_cost_mean_ms\":%.3f,\"layer_cost_p95_ms\":%.3f,\"layer_cost_p99_ms\":%.3f,\"gpu_share\":%.5f,\"flow_share_of_gpu\":%.4f,"
        "\"game_cpu_ms_per_frame\":%.4f,\"presenter_core_share\":%.4f,\"async_gain_upper_bound_ms\":%.3f}\n}\n",
        cost, on.ft_p95 - off.ft_p95, on.ft_p99 - off.ft_p99, gpu, gpu > 0 ? on.gpu_flow_share / gpu : 0.0, on.cpu_game_ms, on.presenter_core, cost);
    j += b;
    return j;
}

}  // namespace stats

// ---------------------------------------------------------------- the layer's side

namespace {
std::mutex g_mu;
stats::Bench g_bench;
unsigned long long g_gen0 = 0;
std::atomic<int> g_bench_active{0};
std::string g_last_state;                              // "", done, aborted (for status.json)

void Finish(bool restore) {
    LiveWritePrivate("bench.json", g_bench.Json());
    const bool complete = g_bench.GetState() == stats::Bench::State::Complete;
    if (restore) Live().enabled = g_bench.Restore();
    g_bench_active = 0;
    g_last_state = complete ? "done" : "aborted";
    FG_LOG("benchmark %s: bench.json written; ENABLED %s", complete ? "complete" : "stopped", restore ? "restored" : "left as the settings file has it");
}
}  // namespace

bool StatsOn() { return Live().stats.load(std::memory_order_relaxed) != 0 || g_bench_active.load(std::memory_order_relaxed) != 0; }
bool StatsWantsSecond() { long long t; return StatsOn() || LiveBenchRequest(&t); }
bool StatsBenchActive() { return g_bench_active.load(std::memory_order_relaxed) != 0; }
const char* StatsBenchLabel() {
    std::lock_guard<std::mutex> l(g_mu);
    return g_bench_active ? g_bench.Label() : g_last_state.c_str();
}

void StatsOnSecond(stats::Record& r, uint64_t) {
    std::lock_guard<std::mutex> l(g_mu);
    LiveSettings& live = Live();
    if (!g_bench.Active()) {
        long long token = 0;
        if (LiveBenchRequest(&token)) {
            LiveBenchAck(token);
            g_bench.Start(live.enabled.load(), live.target_fps.load(), live.max_synthetic.load(), live.base_fps_cap.load(), live.min_gain.load(), live.interp_bidir.load());
            g_gen0 = LiveGeneration();
            g_bench_active = 1; g_last_state.clear();
            live.enabled = 1;                                 // warm-up with the layer on
            FG_LOG("benchmark started: 30 s warm-up, then 3 x (60 s off, 60 s on); do not change settings or leave the game");
            return;                                           // this second belongs to the time before the run
        }
    } else if (LiveGeneration() != g_gen0) {                  // the user changed a setting: their value wins
        g_bench.Abort("a setting was changed during the run");
        Finish(false);
    } else {
        const int next = g_bench.OnSecond(r);
        if (!g_bench.Active()) Finish(true);
        else if (live.enabled.load() != next) { live.enabled = next; FG_LOG("benchmark: layer %s", next ? "on" : "off"); }
    }
    if (StatsOn() || r.bench.size()) {
        timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        (void)LiveStatsAppend(stats::RecordJson(r, (long long)ts.tv_sec));
    }
}

void StatsBenchAbort(const char* reason) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_bench.Active()) return;
    g_bench.Abort(reason);
    Finish(true);
}
