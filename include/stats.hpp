#pragma once
// Measurement logic: VKIFRAME_STATS=1 writes one record per second to stats.jsonl; a benchmark run alternates the layer off and on and writes bench.json.
// Off by default. When off, the only cost is one relaxed atomic load per present (StatsOn()); no timestamps are taken and nothing is written.
// The records and bench.json hold numbers only: no game name, process name, path, host name, user name or IP.
#include <cstdint>
#include <string>
#include <vector>

namespace stats {

// Nearest-rank percentile of V (p in 0..1) of a sorted copy; 0 for an empty set. Defined so it can be recomputed by hand: sort ascending, take element ceil(p*n), counting from 1.
double Percentile(std::vector<double> v, double p);

// Frame intervals of one one-second window, in milliseconds.
struct FrameTimes {
    std::vector<double> ms;
    void Add(double v) { if (ms.size() < 4096) ms.push_back(v); }     // 4096 per second is far above any frame rate; the cap keeps the memory bounded
    void Clear() { ms.clear(); }
    size_t Count() const { return ms.size(); }
    double Mean() const;
    double Max() const;
};

// One second of measurements (one line of stats.jsonl).
struct Record {
    double secs{1.0};                                   // length of the window
    std::string mode;                                   // off / bypass / interp / extrap
    std::string bench;                                  // "" or warmup / settle / off / on while a benchmark runs
    double target_fps{0}, refresh_hz{0}, native_fps{0}, shown_fps{0};
    double gen_per_native{0};                           // k: frames shown beyond the native one (2.0 = 30 -> 90). NOT the number of frames the interpolation pass draws: it draws every shown frame, k + 1 per native frame

    uint64_t frames{0};                                 // native present-to-present intervals in the window
    double ft_mean{0}, ft_p95{0}, ft_p99{0}, ft_max{0}; // game frame time, ms
    double cpu_game_ms{0}, presenter_core{0};           // game-thread CPU ms per frame inside present; presenter thread share of one core (0..1)
    double wait_lock_ms{0}, wait_ring_ms{0}, wait_fence_ms{0}, wait_acquire_ms{0}, wait_present_ms{0};   // game-thread waits per frame
    double flow_ms{0}, pairs_per_s{0}, interp_ms{0}, generated_per_s{0};   // GPU: flow ms per pair, interpolation ms per generated frame, and how many per second
    double gpu_share{0};                                // (pairs_per_s * flow_ms + generated_per_s * interp_ms) / 1000
    double late_mean_ms{0}, late_max_ms{0};             // native frame lateness against its slot (extrapolation mode only, else 0)
    uint64_t cancelled{0}, dropped{0};
};
void FillDerived(Record& r, const FrameTimes& ft);      // frame-time mean/p95/p99/max from the intervals, and gpu_share from the GPU fields
std::string RecordJson(const Record& r, long long unix_s);

// Benchmark: 30 s warm-up (layer on), then reps (6) x (60 s off, 60 s on). The first settle_s seconds after every switch are labelled "settle" and left out of the numbers.
class Bench {
public:
    struct Params { double warmup_s{30}, segment_s{60}, settle_s{2}; int reps{6}; };
    enum class State { Idle, Running, Complete, Aborted };

    Bench() = default;
    explicit Bench(Params p) : p_(p) {}
    // Starts a run. The settings are echoed into bench.json; user_enabled is what to restore.
    void Start(int user_enabled, double target_fps, int max_synthetic, double base_fps_cap, double min_gain, int bidir, int cmd_buffers = 0, int flow_level = 0);
    // Feeds one second. Returns the value ENABLED must have for the next second (1 = layer on, 0 = off). The record's `bench` label is set here.
    int OnSecond(Record& r);
    void Abort(const char* reason);                     // the user changed a setting, or left the game
    State GetState() const { return state_; }
    bool Active() const { return state_ == State::Running; }
    int  Restore() const { return user_enabled_; }      // the ENABLED value to put back when the run ends
    const char* Label() const;                          // warmup / settle / off / on / "" (what the current second is)
    std::string Json() const;                           // bench.json text (valid after Complete or Aborted)

    struct Agg {                                        // one state (off or on), over its counted seconds
        int seconds{0}; uint64_t frames{0};
        double ft_mean{0}, ft_p95{0}, ft_p99{0}, ft_max{0}, native_fps{0}, shown_fps{0};
        double cpu_game_ms{0}, presenter_core{0}, wait_game_ms{0};
        double gpu_share{0}, gpu_flow_share{0}, gpu_interp_share{0};
        double flow_ms{0}, interp_ms{0};
    };
    Agg Aggregate(bool on) const;                       // exposed for the unit test and the Auditor's recomputation
    // Layer cost of each off/on pair (frame-weighted mean frame time of the on segment minus the off segment before it), in ms. Only pairs with frames in both segments.
    std::vector<double> PairCosts() const;
    struct Spread { int n{0}; double mean{0}, se{0}; bool within_noise{true}; };   // mean of the pair costs, standard error of that mean, and whether |mean| < 2 se (n >= 2 only)
    static Spread SpreadOf(const std::vector<double>& pair_costs);

private:
    struct Row { Record r; bool on; int seg; };
    Params p_;
    State state_{State::Idle};
    std::string reason_;
    double elapsed_{0.0};
    int user_enabled_{1};
    int cur_enabled_{1};
    std::vector<Row> rows_;                             // counted seconds only
    double target_{0}, cap_{0}, gain_{0}; int synth_{0}, bidir_{0};
    double refresh_{0};
    int cmd_buffers_{0}, flow_level_{0};                // echoed into bench.json so a file says which settings it was run with (0 = not recorded)
    // where the current second is in the schedule
    int SegmentOf(double t, bool& on, double& into) const;   // -1 warm-up, 0.. segment index
};

}  // namespace stats

// ---- the layer's side (src/stats.cpp) ----
bool StatsOn();                                         // STATS key / environment, or a benchmark running
bool StatsBenchActive();
bool StatsWantsSecond();                                // StatsOn(), or a BENCH request is waiting (so the layer builds this second's record)
// Once a second, from the present path, with this second's record. Writes it, and drives the benchmark (starts one when live.conf asks, switches ENABLED, finishes).
void StatsOnSecond(stats::Record& r, uint64_t now_ns);
void StatsBenchAbort(const char* reason);               // leaving the game
const char* StatsBenchLabel();                          // for status.json: "", warmup, settle, off, on, done, aborted
