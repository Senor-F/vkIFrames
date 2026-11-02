#include "stats.hpp"
#include <cstdarg>

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
    { double ss = 0; for (double v : ft.ms) ss += (v - r.ft_mean) * (v - r.ft_mean); r.ft_std_ms = ft.ms.empty() ? 0.0 : std::sqrt(ss / double(ft.ms.size())); }
    r.gpu_share = (r.pairs_per_s * r.flow_ms + r.generated_per_s * r.interp_ms) / 1000.0;
}

// printf into a std::string of the size it needs: bench.json is written whole, whatever the length of the reason text or the numbers.
static std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string Fmt(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt); va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out;
    if (n > 0) { out.resize(size_t(n) + 1); std::vsnprintf(out.data(), out.size(), fmt, ap2); out.resize(size_t(n)); }
    va_end(ap2);
    return out;
}

std::string RecordJson(const Record& r, long long unix_s) {
    return Fmt(
        "{\"t\":%lld,\"secs\":%.3f,\"mode\":\"%s\",\"bench\":\"%s\",\"target_fps\":%.1f,\"refresh_hz\":%.2f,\"native_fps\":%.2f,\"shown_fps\":%.2f,\"gen_per_native\":%.3f,"
        "\"frames\":%llu,\"ft_mean_ms\":%.3f,\"ft_p95_ms\":%.3f,\"ft_p99_ms\":%.3f,\"ft_max_ms\":%.3f,"
        "\"cpu_game_ms\":%.4f,\"presenter_core\":%.4f,"
        "\"wait_lock_ms\":%.4f,\"wait_ring_ms\":%.4f,\"wait_fence_ms\":%.4f,\"wait_acquire_ms\":%.4f,\"wait_present_ms\":%.4f,"
        "\"flow_ms\":%.4f,\"pairs_per_s\":%.2f,\"interp_ms\":%.4f,\"generated_per_s\":%.2f,\"gpu_share\":%.5f,"
        "\"late_mean_ms\":%.3f,\"late_max_ms\":%.3f,\"cancelled\":%llu,\"dropped\":%llu,\"cap_wait_ms\":%.3f,\"cap_hold_ms\":%.3f,\"ft_std_ms\":%.3f,\"content_delay_ms\":%.3f,\"pid\":%ld}",
        unix_s, r.secs, r.mode.c_str(), r.bench.c_str(), r.target_fps, r.refresh_hz, r.native_fps, r.shown_fps, r.gen_per_native,
        (unsigned long long)r.frames, r.ft_mean, r.ft_p95, r.ft_p99, r.ft_max,
        r.cpu_game_ms, r.presenter_core,
        r.wait_lock_ms, r.wait_ring_ms, r.wait_fence_ms, r.wait_acquire_ms, r.wait_present_ms,
        r.flow_ms, r.pairs_per_s, r.interp_ms, r.generated_per_s, r.gpu_share,
        r.late_mean_ms, r.late_max_ms, (unsigned long long)r.cancelled, (unsigned long long)r.dropped, r.cap_wait_ms, r.cap_hold_ms, r.ft_std_ms, r.content_delay_ms, r.pid);
}

std::string FramesJson(const FrameTimes& ft, long long unix_s, double slot_ms, long pid) {
    std::string s = Fmt("{\"t\":%lld,\"pid\":%ld,\"slot_ms\":%.3f,\"us\":[", unix_s, pid, slot_ms);
    for (size_t i = 0; i < ft.ms.size(); ++i) { char b[24]; std::snprintf(b, sizeof b, i ? ",%lld" : "%lld", (long long)std::llround(ft.ms[i] * 1000.0)); s += b; }
    s += "]}";
    return s;
}

// ---------------------------------------------------------------- benchmark

void Bench::Start(int user_enabled, double target_fps, int max_synthetic, double base_fps_cap, double min_gain, int bidir, int cmd_buffers, int flow_level, double synth_margin, int optical_flow) {
    state_ = State::Running; reason_.clear(); elapsed_ = 0.0; rows_.clear();
    user_enabled_ = user_enabled; cur_enabled_ = 1;
    target_ = target_fps; synth_ = max_synthetic; cap_ = base_fps_cap; gain_ = min_gain; bidir_ = bidir; refresh_ = 0.0; cmd_buffers_ = cmd_buffers; flow_level_ = flow_level; synth_margin_ = synth_margin; optical_flow_ = optical_flow;
}

int Bench::SegmentOf(double t, bool& on, double& into) const {
    if (t < p_.warmup_s) { on = true; into = t; return -1; }
    const double u = t - p_.warmup_s;
    int seg = int(std::floor(u / p_.segment_s));
    seg = std::clamp(seg, 0, 2 * p_.reps - 1);
    on = ((seg % 2) == 1) != (((seg / 2) % 2) == 1);       // pair 0: off, on; pair 1: on, off; pair 2: off, on ... (a steady drift cancels in the mean of the pairs)
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
        bool seg_on; double seg_into;
        rows_.push_back({r, label == "on", SegmentOf(elapsed_, seg_on, seg_into)});
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

std::vector<double> Bench::PairCosts() const {
    std::vector<double> out;
    for (int p = 0; p < p_.reps; ++p) {
        double fo = 0, so = 0, fn = 0, sn = 0;
        for (const Row& w : rows_) {
            if (w.seg != 2 * p && w.seg != 2 * p + 1) continue;
            const double f = double(w.r.frames);
            if (w.on) { fn += f; sn += w.r.ft_mean * f; }
            else      { fo += f; so += w.r.ft_mean * f; }
        }
        if (fo > 0 && fn > 0) out.push_back(sn / fn - so / fo);          // on segment minus off segment, whichever came first
    }
    return out;
}

// Two-sided 95 % Student t for n - 1 degrees of freedom (n = 2 .. 11); the normal value 2.0 beyond that.
static double T95(int n) {
    static const double t[] = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228};
    return (n >= 2 && n <= 11) ? t[n - 2] : 2.0;
}

Bench::Spread Bench::SpreadOf(const std::vector<double>& v) {
    Spread s; s.n = int(v.size());
    if (v.empty()) return s;
    for (double x : v) s.mean += x;
    s.mean /= double(v.size());
    if (v.size() < 2) return s;                                  // one pair: no spread to judge by
    double ss = 0; for (double x : v) ss += (x - s.mean) * (x - s.mean);
    s.se = std::sqrt(ss / double(v.size() - 1)) / std::sqrt(double(v.size()));
    s.within_noise = std::fabs(s.mean) < T95(s.n) * s.se;
    return s;
}

std::string Bench::Json() const {
    const Agg off = Aggregate(false), on = Aggregate(true);
    const bool ok = state_ == State::Complete && off.seconds > 0 && on.seconds > 0;
    auto agg = [](const Agg& a) {
        return Fmt(
            "{\"seconds\":%d,\"frames\":%llu,\"frame_time_mean_ms\":%.3f,\"frame_time_p95_ms\":%.3f,\"frame_time_p99_ms\":%.3f,\"frame_time_max_ms\":%.3f,"
            "\"native_fps\":%.2f,\"shown_fps\":%.2f,\"game_cpu_ms_per_frame\":%.4f,\"presenter_core_share\":%.4f,\"game_wait_ms_per_frame\":%.4f,"
            "\"gpu_share\":%.5f,\"gpu_flow_share\":%.5f,\"gpu_interp_share\":%.5f,\"flow_ms_per_pair\":%.4f,\"interp_ms_per_generated\":%.4f}",
            a.seconds, (unsigned long long)a.frames, a.ft_mean, a.ft_p95, a.ft_p99, a.ft_max, a.native_fps, a.shown_fps, a.cpu_game_ms, a.presenter_core, a.wait_game_ms,
            a.gpu_share, a.gpu_flow_share, a.gpu_interp_share, a.flow_ms, a.interp_ms);
    };
    const char* status = state_ == State::Complete ? "complete" : state_ == State::Aborted ? "aborted" : "idle";
    std::string j = Fmt(
        "{\n\"version\":1,\n\"status\":\"%s\",\n\"reason\":\"%s\",\n"
        "\"notes\":\"generated_per_s and interp_ms count interpolation passes: the pass draws every shown frame (k + 1 per native frame, k = gen_per_native). By default every one of them is a synthetic frame: SYNTH_MARGIN (0.15) keeps t away from 0 and 1, so the frame at the start of a pair is a synthetic one, not the native frame. With SYNTH_MARGIN=0 that frame is the native frame itself and costs no pass. A 60 s segment has about 59 one-second records: 2 settle, about 57 counted, and one at the switch that belongs to neither. layer_cost_pairs_ms is on minus off for each pair; layer_cost_within_noise compares the mean with the 95 %% t interval of the pairs. async_gain_upper_bound_ms equals the layer cost and is NOT an achievable gain: in a GPU-bound game the cost is the layer own GPU time, which a second queue does not remove. With optical_flow 0 the timed pass is the plain cross-fade (flow_ms stays 0, interp_ms is the cross-fade pass); in both modes the capture copy of each game frame and the final copy of each generated frame are NOT timed, so gpu_share and the layer cost from it are a lower bound.\",\n"
        "\"schedule\":{\"warmup_s\":%.0f,\"segment_s\":%.0f,\"repeats\":%d,\"settle_s_left_out_per_segment\":%.0f,\"order\":\"warm-up (layer on), then pairs of segments, off then on in the first pair and on then off in the second, and so on\"},\n"
        "\"settings\":{\"target_fps\":%.1f,\"max_synthetic\":%d,\"base_fps_cap\":%.1f,\"min_gain\":%.3f,\"interp_bidir\":%d,\"cmd_buffers\":%d,\"flow_level\":%d,\"synth_margin\":%.2f,\"optical_flow\":%d,\"refresh_hz\":%.2f,\"base_fps_cap_kept_while_off\":true},\n",
        status, reason_.c_str(), p_.warmup_s, p_.segment_s, p_.reps, p_.settle_s, target_, synth_, cap_, gain_, bidir_, cmd_buffers_, flow_level_, synth_margin_, optical_flow_, refresh_);
    j += "\"off\":" + agg(off) + ",\n\"on\":" + agg(on) + ",\n\"derived\":";
    if (!ok) {
        j += "null\n}\n";
        return j;
    }
    const double cost = on.ft_mean - off.ft_mean;
    const double gpu = on.gpu_share;
    const std::vector<double> pc = PairCosts();
    const Spread sp = SpreadOf(pc);
    std::string pairs = "[";
    for (size_t i = 0; i < pc.size(); ++i) { char t[32]; std::snprintf(t, sizeof t, "%s%.3f", i ? "," : "", pc[i]); pairs += t; }
    pairs += "]";
    char spread[200];
    if (sp.n >= 2) std::snprintf(spread, sizeof spread, "\"layer_cost_pairs_mean_ms\":%.3f,\"layer_cost_se_ms\":%.3f,\"layer_cost_within_noise\":%s", sp.mean, sp.se, sp.within_noise ? "true" : "false");
    else           std::snprintf(spread, sizeof spread, "\"layer_cost_pairs_mean_ms\":%.3f,\"layer_cost_se_ms\":null,\"layer_cost_within_noise\":null", sp.mean);
    j += Fmt(
        "{\"layer_cost_mean_ms\":%.3f,\"layer_cost_p95_ms\":%.3f,\"layer_cost_p99_ms\":%.3f,\"layer_cost_pairs_ms\":%s,%s,\"gpu_share\":%.5f,\"flow_share_of_gpu\":%.4f,"
        "\"game_cpu_ms_per_frame\":%.4f,\"presenter_core_share\":%.4f,\"async_gain_upper_bound_ms\":%.3f}\n}\n",
        cost, on.ft_p95 - off.ft_p95, on.ft_p99 - off.ft_p99, pairs.c_str(), spread, gpu, gpu > 0 ? on.gpu_flow_share / gpu : 0.0, on.cpu_game_ms, on.presenter_core, cost);
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
    const bool written = LiveWritePrivate("bench.json", g_bench.Json());
    if (!written) FG_LOG("benchmark: bench.json could NOT be written (config folder missing, not private or not writable): the numbers are in stats.jsonl; the panel and scripts/vkiframes-bench.sh show an older bench.json or none");
    const bool complete = g_bench.GetState() == stats::Bench::State::Complete;
    if (restore) Live().enabled = g_bench.Restore();
    g_bench_active = 0; LiveBenchRunning(false);
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
        (void)LiveBenchCancel();                              // a cancel with nothing running is not kept for the next run
        if (LiveBenchRequest(&token)) {
            LiveBenchAck(token);
            g_bench.Start(live.enabled.load(), live.target_fps.load(), live.max_synthetic.load(), live.base_fps_cap.load(), live.min_gain.load(), live.interp_bidir.load(), live.cmd_buffers.load(), GetConfig().flow_level, live.synth_margin.load(), GetConfig().interpolate ? 1 : 0);
            g_gen0 = LiveGeneration();
            g_bench_active = 1; LiveBenchRunning(true); g_last_state.clear();
            live.enabled = 1;                                 // warm-up with the layer on
            FG_LOG("benchmark started: 30 s warm-up, then 6 pairs of 60 s (off, on; every second pair on, off), about 13 minutes; do not change settings or leave the game");
            return;                                           // this second belongs to the time before the run
        }
    } else {
        long long ignored = 0;
        if (LiveBenchRequest(&ignored)) {                     // a new number while a run is going: neither a second run afterwards nor a stop
            LiveBenchAck(ignored);
            FG_LOG("benchmark: request %lld ignored, a run is going (ask again when it is done)", ignored);
        }
        if (LiveBenchCancel()) {                              // bench.req was set to 0 (scripts/vkiframes-bench.sh --cancel)
            g_bench.Abort("cancelled (bench.req set to 0)");
            Finish(true);
        } else if (LiveGeneration() != g_gen0) {              // the user changed a setting: their value wins
            g_bench.Abort("a setting was changed during the run");
            Finish(false);
        } else {
            const int next = g_bench.OnSecond(r);
            if (!g_bench.Active()) Finish(true);
            else if (live.enabled.load() != next) { live.enabled = next; FG_LOG("benchmark: layer %s", next ? "on" : "off"); }
        }
    }
    if (StatsOn() || r.bench.size()) {
        timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        (void)LiveStatsAppend(stats::RecordJson(r, (long long)ts.tv_sec), g_bench_active.load() == 0);      // no rotation inside a benchmark run: its records stay in one file
    }
}

void StatsBenchAbort(const char* reason) {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_bench.Active()) return;
    g_bench.Abort(reason);
    Finish(true);
}
