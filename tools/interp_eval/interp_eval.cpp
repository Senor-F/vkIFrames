// Offline scoring of interpolation methods against a real middle frame. No GPU needed.
//
//   interp_eval A.rgba16f TRUTH.rgba16f B.rgba16f WIDTH HEIGHT [out_prefix] [field_level 1|2]
//
// A, TRUTH, B are three consecutive native frames captured with VKIFRAME_DUMP (pairN_A.rgba16f, raw RGBA half floats). The methods
// interpolate between A and B at t = 0.5 and are scored against TRUTH. With out_prefix set, each result and its error image are written as
// PPM files. The flow comes from include/flow_ref.hpp, the same algorithm as the shaders.
#include "flow_ref.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace flowref;

struct Rgb { int w{0}, h{0}; std::vector<float> v; float* px(int x, int y) { return &v[(size_t(y) * w + x) * 3]; } const float* px(int x, int y) const { return &v[(size_t(y) * w + x) * 3]; } };

static float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float f;
    if (e == 0) f = std::ldexp(float(m), -24);
    else if (e == 31) f = m ? NAN : INFINITY;
    else f = std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -f : f;
}

static bool Load(const char* path, int w, int h, Rgb& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return false; }
    std::vector<uint16_t> raw(size_t(w) * h * 4);
    const size_t n = std::fread(raw.data(), 2, raw.size(), f);
    std::fclose(f);
    if (n != raw.size()) { std::fprintf(stderr, "%s: expected %zu half floats, got %zu (wrong size?)\n", path, raw.size(), n); return false; }
    out.w = w; out.h = h; out.v.resize(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c) out.v[i * 3 + c] = std::clamp(HalfToFloat(raw[i * 4 + c]), 0.0f, 1.0f);
    return true;
}

static Img Luma(const Rgb& s) {
    Img l(s.w, s.h);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) { const float* p = s.px(x, y); l.at(x, y) = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }
    return l;
}

static void Sample(const Rgb& s, float x, float y, float* out) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = int(fx), iy = int(fy);
    const float tx = x - fx, ty = y - fy;
    const int x0 = std::clamp(ix, 0, s.w - 1), x1 = std::clamp(ix + 1, 0, s.w - 1), y0 = std::clamp(iy, 0, s.h - 1), y1 = std::clamp(iy + 1, 0, s.h - 1);
    for (int c = 0; c < 3; ++c)
        out[c] = (s.px(x0, y0)[c] * (1 - tx) + s.px(x1, y0)[c] * tx) * (1 - ty) + (s.px(x0, y1)[c] * (1 - tx) + s.px(x1, y1)[c] * tx) * ty;
}

// the vector field at a full-resolution pixel: bilinear between the four nearest field pixels, weighted by luma closeness to `guide`
// (include/flow_ref.hpp InterpPixel; sigma <= 0: plain bilinear)
struct Vec { float x, y, cost, mask; };
static Vec FieldAt(const Flow& f, const std::vector<float>& mask, const Img& guide, int x, int y, float scale, float sigma) {
    const float fx = (float(x) + 0.5f) / scale - 0.5f, fy = (float(y) + 0.5f) / scale - 0.5f;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const float tx = fx - float(ix), ty = fy - float(iy);
    const float gp = guide.get(x, y);
    float vx = 0, vy = 0, cost = 0, ws = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            float w = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            const size_t c = f.idx(std::clamp(ix + i, 0, f.w - 1), std::clamp(iy + j, 0, f.h - 1));
            if (sigma > 0) {
                const int gx = int(std::lround((float(ix + i) + 0.5f) * scale - 0.5f)), gy = int(std::lround((float(iy + j) + 0.5f) * scale - 0.5f));
                const float d = guide.get(gx, gy) - gp;
                w *= 0.02f + std::exp(-d * d / (2 * sigma * sigma));
            }
            vx += w * f.x[c]; vy += w * f.y[c]; cost += w * f.cost[c]; ws += w;
        }
    const float inv = 1.0f / std::max(ws, 1e-6f);
    const size_t q = f.idx(std::clamp(int(std::floor(fx + 0.5f)), 0, f.w - 1), std::clamp(int(std::floor(fy + 0.5f)), 0, f.h - 1));
    return {vx * inv * scale, vy * inv * scale, cost * inv, mask.empty() ? 0.0f : mask[q]};
}

struct Ctx {
    const Rgb &a, &b;
    Img la, lb;
    Flow fwd, bwd;
    std::vector<float> mask_f, mask_b;
    float scale;
    float pan_k{0.0f};   // frame-level cross-fade weight (see FrameBad)
};

// share of the flow field that is occluded or badly matched: the single number the pan fallback decides on
static float FrameBad(const Ctx& c) {
    double sum = 0.0; size_t n = 0;
    for (int y = 0; y < c.fwd.h; ++y)
        for (int x = 0; x < c.fwd.w; ++x) {
            const size_t q = size_t(y) * c.fwd.w + x;
            sum += std::max(c.mask_f[q], Smoothstep(0.06f, 0.14f, c.fwd.cost[q])); ++n;
        }
    return float(sum / double(n));
}

using Method = std::function<void(const Ctx&, float t, Rgb&)>;

static void Fade(const Ctx& c, float t, Rgb& out) {
    for (size_t i = 0; i < out.v.size(); ++i) out.v[i] = (1 - t) * c.a.v[i] + t * c.b.v[i];
}

// the method in the layer today: one vector per output pixel from the forward field, both frames sampled along it, faded where the match is poor
// fb: what replaces a rejected interpolation (0 = cross-fade, 1 = frame A, 2 = frame B); lean: where the two moved samples disagree, blend toward frame A (1) or B (2) (0 = off).
// Both mirror the Auditor's findings 2 and 1; the evaluation time t is 0.5, so "nearest frame" is shown for both choices.
// ui_thr > 0: static-UI mask. Where the 3x3-averaged luma of A and B agrees within ui_thr (the averaging takes the film grain out), the pixel
// is taken as unmoving UI and gets the plain cross-fade; the weight ramps to zero by 2*ui_thr.
struct Variant { int fb{0}; int lean{0}; float pan_k{0.0f}; float lean_lo{0.15f}, lean_hi{0.6f}; float ui_thr{0.0f}; };
static float Luma3x3(const Rgb& s, int x, int y) {
    float acc = 0.0f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const float* p = s.px(std::clamp(x + dx, 0, s.w - 1), std::clamp(y + dy, 0, s.h - 1));
            acc += 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        }
    return acc / 9.0f;
}
// UI_MODE=1 (env): mean of |A-B| over 5x5 instead of the difference of the 3x3 means; a moving edge cannot cancel out in it
static float StaticWeight(const Ctx& c, int x, int y, float thr) {
    static const bool abs_mode = std::getenv("UI_MODE") && std::atoi(std::getenv("UI_MODE")) == 1;
    float d;
    if (abs_mode) {
        static const int R = std::getenv("UI_R") ? std::atoi(std::getenv("UI_R")) : 1;
        float acc = 0.0f;
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) {
                const int xx = std::clamp(x + dx, 0, c.a.w - 1), yy = std::clamp(y + dy, 0, c.a.h - 1);
                const float* p = c.a.px(xx, yy); const float* q = c.b.px(xx, yy);
                acc += std::fabs(0.299f * (p[0] - q[0]) + 0.587f * (p[1] - q[1]) + 0.114f * (p[2] - q[2]));
            }
        d = acc / float((2 * R + 1) * (2 * R + 1));
    } else d = std::fabs(Luma3x3(c.a, x, y) - Luma3x3(c.b, x, y));
    return 1.0f - Smoothstep(thr, 2.0f * thr, d);
}
static void Run(const Ctx& c, float t, Rgb& out, const Variant& vr) {
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const Vec v = FieldAt(c.fwd, c.mask_f, c.la, x, y, c.scale, 0.08f);
            float k = std::max(v.mask, Smoothstep(0.06f, 0.14f, v.cost));
            k = std::max(k, Smoothstep(kFastLow, kFastHigh, std::hypot(v.x, v.y)));
            k = std::max(k, vr.pan_k);
            if (vr.ui_thr > 0.0f) k = std::max(k, StaticWeight(c, x, y, vr.ui_thr));
            float pa[3], pb[3];
            Sample(c.a, float(x) - t * v.x, float(y) - t * v.y, pa);
            Sample(c.b, float(x) + (1 - t) * v.x, float(y) + (1 - t) * v.y, pb);
            const float* fa = c.a.px(x, y); const float* fb = c.b.px(x, y);
            float lean_w = 0.0f;
            if (vr.lean) {
                const float la = 0.299f * pa[0] + 0.587f * pa[1] + 0.114f * pa[2], lb = 0.299f * pb[0] + 0.587f * pb[1] + 0.114f * pb[2];
                lean_w = Smoothstep(vr.lean_lo, vr.lean_hi, std::fabs(la - lb));
            }
            for (int ch = 0; ch < 3; ++ch) {
                float moved = (1 - t) * pa[ch] + t * pb[ch];
                if (vr.lean) moved += ((vr.lean == 1 ? fa[ch] : fb[ch]) - moved) * lean_w;
                const float fd = vr.fb == 0 ? (1 - t) * fa[ch] + t * fb[ch] : (vr.fb == 1 ? fa[ch] : fb[ch]);
                out.px(x, y)[ch] = moved + (fd - moved) * k;
            }
        }
}
static void CurrentK(const Ctx& c, float t, Rgb& out, float pan_k) { Variant v; v.pan_k = pan_k; Run(c, t, out, v); }
static void NearA(const Ctx& c, float t, Rgb& out) { Variant v; v.fb = 1; Run(c, t, out, v); }
static void NearB(const Ctx& c, float t, Rgb& out) { Variant v; v.fb = 2; Run(c, t, out, v); }
static void LeanA(const Ctx& c, float t, Rgb& out) { Variant v; v.lean = 1; Run(c, t, out, v); }
static void LeanB(const Ctx& c, float t, Rgb& out) { Variant v; v.lean = 2; Run(c, t, out, v); }
static void UiMask(const Ctx& c, float t, Rgb& out, float thr) { Variant v; v.pan_k = c.pan_k; v.ui_thr = thr; Run(c, t, out, v); }
// Extrapolation (no added latency): the newest frame B moved on along its own backward flow, as if the motion A -> B continued for s more intervals.
// Content at x in B was at x + v_b(x) in A, so it will be at x - s * v_b(x); sampled backward: out(x) = B(x + s * v_b(x)). The truth frame is
// B + s * (B - A) in time (EXTRAP_S, default 1: A, B, TRUTH consecutive; use A = frame 0, B = frame 2, TRUTH = frame 3 for s = 0.5).
// Weight falls back to plain B where B's backward match is poor, the pixel is occluded, or the motion is fast; full_pan: the whole frame too.
static void Extrapolate(const Ctx& c, float, Rgb& out, bool use_mask, bool use_pan) {
    static const float s = std::getenv("EXTRAP_S") ? float(std::atof(std::getenv("EXTRAP_S"))) : 1.0f;
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const Vec v = FieldAt(c.bwd, c.mask_b, c.lb, x, y, c.scale, 0.08f);
            float k = Smoothstep(0.06f, 0.14f, v.cost);
            if (use_mask) k = std::max(k, v.mask);
            k = std::max(k, Smoothstep(kFastLow, kFastHigh, std::hypot(v.x, v.y)));
            k = std::max(k, 1.0f - Smoothstep(0.4f, 1.2f, std::hypot(v.x, v.y)));   // EXTRAP_DEAD_LO/HI in the shader: tiny vectors are flow noise
            if (use_pan) k = std::max(k, c.pan_k);
            float p[3];
            Sample(c.b, float(x) + s * v.x, float(y) + s * v.y, p);
            const float* fb = c.b.px(x, y);
            for (int ch = 0; ch < 3; ++ch) out.px(x, y)[ch] = p[ch] + (fb[ch] - p[ch]) * k;
        }
}
static void Current(const Ctx& c, float t, Rgb& out) { CurrentK(c, t, out, 0.0f); }
static void Adaptive(const Ctx& c, float t, Rgb& out) { CurrentK(c, t, out, c.pan_k); }

// Two interpolations, each from its own flow: A moved forward by t along A->B, B moved back by (1-t) along B->A. Each is trusted by its own match
// cost and occlusion mask; where one is bad the other carries the pixel, where both are bad the cross-fade does.
static void Bidirectional(const Ctx& c, float t, Rgb& out) {
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const Vec vf = FieldAt(c.fwd, c.mask_f, c.la, x, y, c.scale, 0.08f);   // A -> B, defined on A's grid
            const Vec vb = FieldAt(c.bwd, c.mask_b, c.lb, x, y, c.scale, 0.08f);   // B -> A, defined on B's grid
            const float cf = (1.0f - std::max(vf.mask, Smoothstep(0.06f, 0.14f, vf.cost))) * (1.0f - Smoothstep(kFastLow, kFastHigh, std::hypot(vf.x, vf.y)));
            const float cb = (1.0f - std::max(vb.mask, Smoothstep(0.06f, 0.14f, vb.cost))) * (1.0f - Smoothstep(kFastLow, kFastHigh, std::hypot(vb.x, vb.y)));
            float pa[3], pb[3];
            Sample(c.a, float(x) - t * vf.x, float(y) - t * vf.y, pa);
            Sample(c.b, float(x) - (1 - t) * vb.x, float(y) - (1 - t) * vb.y, pb);
            const float wa = (1 - t) * cf, wb = t * cb;
            const float* fa = c.a.px(x, y); const float* fb = c.b.px(x, y);
            const float wsum = wa + wb, trust = std::min(1.0f, wsum / std::max(1e-6f, std::max(1 - t, t)));      // 1 when the stronger frame is fully trusted
            for (int ch = 0; ch < 3; ++ch) {
                const float fd = (1 - t) * fa[ch] + t * fb[ch];
                const float moved = wsum > 1e-6f ? (wa * pa[ch] + wb * pb[ch]) / wsum : fd;
                out.px(x, y)[ch] = fd + (moved - fd) * trust;
            }
        }
}

// One-sided fill (VKIFRAME_ONESIDED_FILL, experimental; mirrors apply_fill in shaders/flow_interp.comp; the "fill_consist" variant of the offline experiment).
// Where the forward/backward check says a pixel is occluded, it is taken from the one frame that shows the background, along the best consistent vector of
// that frame's flow within kFillR field pixels, and blended into the bidirectional result by the occlusion weight.
constexpr int   kFillR = 6;
constexpr float kFillOccLo = 0.1f, kFillOccHi = 0.5f, kFillMaskMax = 0.3f, kFillCostMax = 0.10f;
static bool FillBgVector(const Flow& f, const std::vector<float>& mask, int qx, int qy, float& vx, float& vy, float& bad) {
    float best = 1e9f; bool found = false; vx = vy = 0.0f; bad = 9.0f;
    for (int dy = -kFillR; dy <= kFillR; ++dy) for (int dx = -kFillR; dx <= kFillR; ++dx) {
        const int x = qx + dx, y = qy + dy; if (x < 0 || y < 0 || x >= f.w || y >= f.h) continue;
        const size_t i = f.idx(x, y); if (mask[i] > kFillMaskMax || f.cost[i] > kFillCostMax) continue;
        const float score = mask[i] * 4.0f + f.cost[i] * 10.0f + 0.01f * std::hypot(f.x[i], f.y[i]);
        if (score < best) { best = score; vx = f.x[i]; vy = f.y[i]; bad = mask[i] + f.cost[i]; found = true; }
    }
    return found;
}
static void FillConsistRef(const Ctx& c, float t, Rgb& out) {
    Bidirectional(c, t, out);
    for (int y = 0; y < out.h; ++y) for (int x = 0; x < out.w; ++x) {
        const int qx = std::clamp(int(std::floor((float(x) + 0.5f) / c.scale)), 0, c.fwd.w - 1), qy = std::clamp(int(std::floor((float(y) + 0.5f) / c.scale)), 0, c.fwd.h - 1);
        const float occ_f = c.mask_f[c.fwd.idx(qx, qy)], occ_b = c.mask_b[c.bwd.idx(qx, qy)];
        const float occ = Smoothstep(kFillOccLo, kFillOccHi, std::max(occ_f, occ_b));
        if (occ <= 0.0f) continue;
        float ax, ay, ab, bx, by, bb;
        const bool fa = FillBgVector(c.fwd, c.mask_f, qx, qy, ax, ay, ab), fb = FillBgVector(c.bwd, c.mask_b, qx, qy, bx, by, bb);
        if (!fa && !fb) continue;
        const bool use_b = fb && (!fa || bb < ab - 1e-3f || (std::fabs(bb - ab) <= 1e-3f && occ_b >= occ_f));
        float col[3];
        if (use_b) Sample(c.b, float(x) - (1 - t) * bx * c.scale, float(y) - (1 - t) * by * c.scale, col);
        else       Sample(c.a, float(x) - t * ax * c.scale, float(y) - t * ay * c.scale, col);
        for (int ch = 0; ch < 3; ++ch) out.px(x, y)[ch] += (col[ch] - out.px(x, y)[ch]) * occ;
    }
}

// Auditor question 1: the trust of the strongest side alone, so a pixel only B can explain is a full B interpolation at t = 0.25 too
static void BidirectionalOneSided(const Ctx& c, float t, Rgb& out) {
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const Vec vf = FieldAt(c.fwd, c.mask_f, c.la, x, y, c.scale, 0.08f);   // A -> B, defined on A's grid
            const Vec vb = FieldAt(c.bwd, c.mask_b, c.lb, x, y, c.scale, 0.08f);   // B -> A, defined on B's grid
            const float cf = (1.0f - std::max(vf.mask, Smoothstep(0.06f, 0.14f, vf.cost))) * (1.0f - Smoothstep(kFastLow, kFastHigh, std::hypot(vf.x, vf.y)));
            const float cb = (1.0f - std::max(vb.mask, Smoothstep(0.06f, 0.14f, vb.cost))) * (1.0f - Smoothstep(kFastLow, kFastHigh, std::hypot(vb.x, vb.y)));
            float pa[3], pb[3];
            Sample(c.a, float(x) - t * vf.x, float(y) - t * vf.y, pa);
            Sample(c.b, float(x) - (1 - t) * vb.x, float(y) - (1 - t) * vb.y, pb);
            const float wa = (1 - t) * cf, wb = t * cb;
            const float* fa = c.a.px(x, y); const float* fb = c.b.px(x, y);
            const float wsum = wa + wb, trust = std::max(cf, cb);      // 1 when the stronger frame is fully trusted
            for (int ch = 0; ch < 3; ++ch) {
                const float fd = (1 - t) * fa[ch] + t * fb[ch];
                const float moved = wsum > 1e-6f ? (wa * pa[ch] + wb * pb[ch]) / wsum : fd;
                out.px(x, y)[ch] = fd + (moved - fd) * trust;
            }
        }
}

// Forward splatting: every pixel of A is pushed to where it will be at t, every pixel of B to where it was. Overlaps are resolved by
// motion size (the faster thing is in front), what nothing reaches is filled with the cross-fade.
static void Splat(const Ctx& c, float t, Rgb& out) {
    std::vector<float> acc(size_t(out.w) * out.h * 3, 0.0f), wacc(size_t(out.w) * out.h, 0.0f);
    auto put = [&](float px, float py, const float* col, float w) {
        const int ix = int(std::floor(px)), iy = int(std::floor(py));
        const float tx = px - float(ix), ty = py - float(iy);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int xx = ix + i, yy = iy + j;
                if (xx < 0 || yy < 0 || xx >= out.w || yy >= out.h) continue;
                const float ww = w * (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                const size_t o = size_t(yy) * out.w + xx;
                for (int ch = 0; ch < 3; ++ch) acc[o * 3 + ch] += ww * col[ch];
                wacc[o] += ww;
            }
    };
    const float zk = 0.15f;                                                       // how strongly motion size decides who is in front
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const Vec vf = FieldAt(c.fwd, c.mask_f, c.la, x, y, c.scale, 0.08f);
            const float trf = 1.0f - std::max(vf.mask, Smoothstep(0.06f, 0.14f, vf.cost));
            put(float(x) + t * vf.x, float(y) + t * vf.y, c.a.px(x, y), (1 - t) * trf * std::exp(zk * std::min(std::hypot(vf.x, vf.y), 40.0f)));
            const Vec vb = FieldAt(c.bwd, c.mask_b, c.lb, x, y, c.scale, 0.08f);
            const float trb = 1.0f - std::max(vb.mask, Smoothstep(0.06f, 0.14f, vb.cost));
            put(float(x) + (1 - t) * vb.x, float(y) + (1 - t) * vb.y, c.b.px(x, y), t * trb * std::exp(zk * std::min(std::hypot(vb.x, vb.y), 40.0f)));
        }
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const size_t o = size_t(y) * out.w + x;
            const float* fa = c.a.px(x, y); const float* fb = c.b.px(x, y);
            const float cover = std::min(1.0f, wacc[o] / 0.35f);                    // weight of one fully trusted frame at t = 0.5 is 0.5 x 1 (splat footprints spread it)
            for (int ch = 0; ch < 3; ++ch) {
                const float fd = (1 - t) * fa[ch] + t * fb[ch];
                const float sp = wacc[o] > 1e-6f ? acc[o * 3 + ch] / wacc[o] : fd;
                out.px(x, y)[ch] = fd + (sp - fd) * cover;
            }
        }
}

struct Score { double mae, mae_hard, psnr, hard_frac; };
static Score Measure(const Rgb& r, const Rgb& truth, const Rgb& a, const Rgb& b) {
    double sum = 0, sum_h = 0, se = 0; size_t nh = 0;
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            double e = 0;
            for (int ch = 0; ch < 3; ++ch) { const double d = r.px(x, y)[ch] - truth.px(x, y)[ch]; e += std::fabs(d); se += d * d; }
            e /= 3; sum += e;
            const float* pa = a.px(x, y); const float* pb = b.px(x, y);       // "hard" pixel: A and B disagree there, so the interpolation matters
            const double dab = (std::fabs(pa[0] - pb[0]) + std::fabs(pa[1] - pb[1]) + std::fabs(pa[2] - pb[2])) / 3;
            if (dab > 0.04) { sum_h += e; ++nh; }
        }
    const double n = double(r.w) * r.h;
    return {sum / n, nh ? sum_h / double(nh) : 0.0, 10.0 * std::log10(1.0 / std::max(se / (n * 3), 1e-12)), double(nh) / n};
}

// mean absolute error inside a rectangle (the HUD), env HUD="x0,y0,x1,y1"
static double RegionMae(const Rgb& r, const Rgb& truth, int x0, int y0, int x1, int y1) {
    double sum = 0; size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) { for (int ch = 0; ch < 3; ++ch) sum += std::fabs(r.px(x, y)[ch] - truth.px(x, y)[ch]); n += 3; }
    return n ? sum / double(n) : 0.0;
}

static void WritePpm(const std::string& path, const Rgb& r, float gain = 1.0f, const Rgb* ref = nullptr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", r.w, r.h);
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                float v = r.px(x, y)[ch];
                if (ref) v = std::fabs(v - ref->px(x, y)[ch]) * gain;
                std::fputc(int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f), f);
            }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: %s A TRUTH B WIDTH HEIGHT [out_prefix] [field_level 1|2]\n", argv[0]); return 2; }
    const int w = std::atoi(argv[4]), h = std::atoi(argv[5]);
    const std::string prefix = argc > 6 ? argv[6] : "";
    const int level = argc > 7 ? std::clamp(std::atoi(argv[7]), 1, 2) : 2;
    Rgb a, truth, b;
    if (!Load(argv[1], w, h, a) || !Load(argv[2], w, h, truth) || !Load(argv[3], w, h, b)) return 1;

    Ctx c{a, b, Luma(a), Luma(b), {}, {}, {}, {}, 0};
    const Pyramid pa = BuildPyramid(c.la), pb = BuildPyramid(c.lb);
    c.fwd = Estimate(pa, pb, level - 1, true);
    c.bwd = Estimate(pb, pa, level - 1, true);
    c.mask_f = Occlusion(c.fwd, c.bwd);
    c.mask_b = Occlusion(c.bwd, c.fwd);
    c.scale = float(1 << level);
    const float bad = FrameBad(c);
    const float lo = std::getenv("PAN_LO") ? float(std::atof(std::getenv("PAN_LO"))) : 0.15f, hi = std::getenv("PAN_HI") ? float(std::atof(std::getenv("PAN_HI"))) : 0.35f;
    c.pan_k = Smoothstep(lo, hi, bad);
    std::printf("bad share of the field %.3f -> pan fallback weight %.2f (PAN_LO %.2f, PAN_HI %.2f)\n", double(bad), double(c.pan_k), double(lo), double(hi));

    const float eval_t = std::getenv("EVAL_T") ? float(std::atof(std::getenv("EVAL_T"))) : 0.5f;   // time of TRUTH between A (0) and B (1)
    struct M { const char* name; Method fn; } methods[] = {
        {"fade", Fade}, {"current", Current}, {"bidirectional", Bidirectional}, {"bidir_onesided", BidirectionalOneSided}, {"bidir_fill", FillConsistRef}, {"splat", Splat}, {"adaptive", Adaptive}, {"near_A", NearA}, {"near_B", NearB}, {"lean_A", LeanA}, {"lean_B", LeanB},
        {"extrap", [](const Ctx& c, float t, Rgb& o) { Extrapolate(c, t, o, true, false); }}, {"extrap_pan", [](const Ctx& c, float t, Rgb& o) { Extrapolate(c, t, o, true, true); }}, {"extrap_nomask", [](const Ctx& c, float t, Rgb& o) { Extrapolate(c, t, o, false, false); }},
        {"ui_0.03", [](const Ctx& c, float t, Rgb& o) { UiMask(c, t, o, 0.03f); }}, {"ui_0.06", [](const Ctx& c, float t, Rgb& o) { UiMask(c, t, o, 0.06f); }}, {"ui_0.10", [](const Ctx& c, float t, Rgb& o) { UiMask(c, t, o, 0.10f); }},
    };
    const Score s_a = Measure(a, truth, a, b), s_b = Measure(b, truth, a, b);
    std::printf("frames %dx%d, flow field at level %d (%dx%d), %.1f%% of pixels differ between A and B\n", w, h, level, c.fwd.w, c.fwd.h, 100.0 * s_a.hard_frac);
    int hud[4] = {0, 0, 0, 0};
    const bool have_hud = std::getenv("HUD") && std::sscanf(std::getenv("HUD"), "%d,%d,%d,%d", &hud[0], &hud[1], &hud[2], &hud[3]) == 4;
    std::printf("%-14s %9s %9s %8s %9s\n", "method", "MAE all", "MAE hard", "PSNR dB", have_hud ? "MAE HUD" : "");
    std::printf("%-14s %9.5f %9.5f %8.2f\n", "A as is", s_a.mae, s_a.mae_hard, s_a.psnr);
    std::printf("%-14s %9.5f %9.5f %8.2f\n", "B as is", s_b.mae, s_b.mae_hard, s_b.psnr);
    for (auto& m : methods) {
        Rgb out; out.w = w; out.h = h; out.v.assign(size_t(w) * h * 3, 0.0f);
        m.fn(c, eval_t, out);
        const Score s = Measure(out, truth, a, b);
        std::printf("%-14s %9.5f %9.5f %8.2f", m.name, s.mae, s.mae_hard, s.psnr);
        if (have_hud) std::printf(" %9.5f", RegionMae(out, truth, hud[0], hud[1], hud[2], hud[3]));
        std::printf("\n");
        if (!prefix.empty()) { WritePpm(prefix + "_" + m.name + ".ppm", out); WritePpm(prefix + "_" + m.name + "_err.ppm", out, 4.0f, &truth); }
    }
    if (!prefix.empty()) { WritePpm(prefix + "_truth.ppm", truth); WritePpm(prefix + "_A.ppm", a); WritePpm(prefix + "_B.ppm", b); }
    return 0;
}
