#pragma once
// Stage 4: CPU reference of the optical-flow passes (shaders/flow_*.comp). The shaders and this file implement the same
// algorithm step by step, so the algorithm can be checked against known motion without a GPU (tests/test_flow_ref.cpp).
// Units: a flow vector is in pixels of the level it belongs to. Vectors point from A to B: A(p) ~ B(p + v).

#include <algorithm>
#include <cmath>
#include <vector>

namespace flowref {

constexpr int   kLevels        = 4;        // pyramid levels 1..4 (1/2 .. 1/16); the flow field is estimated at level 1 (half resolution)
constexpr int   kCoarseRadius  = 4;        // search radius at the coarsest level, in that level's pixels
constexpr int   kRadius[kLevels] = {1, 1, 2, 2};  // refine search radius at levels 1..4 (level 4 is the exhaustive coarse search); each level only corrects the half-pixel error left by the coarser one
constexpr int   kPatch[kLevels] = {1, 2, 3, 3};   // patch radius at levels 1..4 (3x3 .. 7x7); coarse levels are small, so large patches are cheap
constexpr float kTieBias       = 1e-4f;    // coarse search: prefers small vectors in flat regions
constexpr float kZeroMargin    = 2e-3f;    // a non-zero vector must beat the zero vector by this much (mean abs luma): flat areas snap to 0
constexpr float kLambda        = 2e-4f;    // refine: penalty on moving away from the pixel's own coarse vector
constexpr float kOcclThreshold = 1.5f;     // forward/backward disagreement (level-1 pixels) where the mask starts to rise

struct Img {
    int w{0}, h{0};
    std::vector<float> v;
    Img() = default;
    Img(int w_, int h_) : w(w_), h(h_), v(size_t(w_) * h_, 0.0f) {}
    float& at(int x, int y) { return v[size_t(y) * w + x]; }
    float get(int x, int y) const { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); return v[size_t(y) * w + x]; }
};

struct Flow {
    int w{0}, h{0};
    std::vector<float> x, y, cost;
    Flow() = default;
    Flow(int w_, int h_) : w(w_), h(h_), x(size_t(w_) * h_, 0.0f), y(size_t(w_) * h_, 0.0f), cost(size_t(w_) * h_, 0.0f) {}
    size_t idx(int px, int py) const { return size_t(py) * w + px; }
};

// flow_luma.comp / flow_down.comp: 2x2 box average, sizes round up, reads clamp to the edge.
inline Img Down(const Img& s) {
    Img d((s.w + 1) / 2, (s.h + 1) / 2);
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x)
            d.at(x, y) = 0.25f * (s.get(2 * x, 2 * y) + s.get(2 * x + 1, 2 * y) + s.get(2 * x, 2 * y + 1) + s.get(2 * x + 1, 2 * y + 1));
    return d;
}

// Mean absolute difference over a (2r+1)^2 patch; coordinates clamp to the edge.
inline float PatchCost(const Img& a, const Img& b, int ax, int ay, int bx, int by, int r) {
    float sum = 0.0f;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) sum += std::fabs(a.get(ax + dx, ay + dy) - b.get(bx + dx, by + dy));
    const int n = 2 * r + 1;
    return sum / float(n * n);
}

// flow_coarse.comp: exhaustive search in +-kCoarseRadius at the coarsest level.
inline Flow Coarse(const Img& a, const Img& b) {
    Flow f(a.w, a.h);
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            float best = 1e30f; int bx = 0, by = 0;
            for (int dy = -kCoarseRadius; dy <= kCoarseRadius; ++dy)
                for (int dx = -kCoarseRadius; dx <= kCoarseRadius; ++dx) {
                    const int cx = x + dx, cy = y + dy;
                    if (cx < 0 || cy < 0 || cx >= a.w || cy >= a.h) continue;
                    const float c = PatchCost(a, b, x, y, cx, cy, kPatch[kLevels - 1]) + kTieBias * float(dx * dx + dy * dy) + ((dx || dy) ? kZeroMargin : 0.0f);
                    if (c < best) { best = c; bx = dx; by = dy; }
                }
            const size_t i = f.idx(x, y);
            f.x[i] = float(bx); f.y[i] = float(by); f.cost[i] = best;
        }
    return f;
}

// flow_refine.comp: candidates are the pixel's own upsampled coarse vector and those of the three nearest other coarse pixels
// (so a motion boundary can pick the vector of the right side), then +-1 pixel around the best of them.
inline Flow Refine(const Img& a, const Img& b, const Flow& coarse, int patch, int radius, bool subpixel = false) {
    Flow f(a.w, a.h);
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const int cx0 = std::min(x / 2, coarse.w - 1), cy0 = std::min(y / 2, coarse.h - 1);
            const int sx = (x & 1) ? 1 : -1, sy = (y & 1) ? 1 : -1;
            const int cxs[4] = {cx0, std::clamp(cx0 + sx, 0, coarse.w - 1), cx0, std::clamp(cx0 + sx, 0, coarse.w - 1)};
            const int cys[4] = {cy0, cy0, std::clamp(cy0 + sy, 0, coarse.h - 1), std::clamp(cy0 + sy, 0, coarse.h - 1)};
            int bvx[4], bvy[4];
            for (int i = 0; i < 4; ++i) {
                const size_t ci = coarse.idx(cxs[i], cys[i]);
                bvx[i] = int(std::lround(coarse.x[ci] * 2.0f)); bvy[i] = int(std::lround(coarse.y[ci] * 2.0f));
            }
            auto cost = [&](int vx, int vy) {
                const float ox = float(vx - bvx[0]), oy = float(vy - bvy[0]);
                return PatchCost(a, b, x, y, x + vx, y + vy, patch) + kLambda * (ox * ox + oy * oy) + ((vx || vy) ? kZeroMargin : 0.0f);
            };
            float best = 1e30f; int mx = bvx[0], my = bvy[0];
            for (int i = 0; i < 4; ++i) { const float c = cost(bvx[i], bvy[i]); if (c < best) { best = c; mx = bvx[i]; my = bvy[i]; } }
            const int cx = mx, cy = my;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (!dx && !dy) continue;
                    const float c = cost(cx + dx, cy + dy);
                    if (c < best) { best = c; mx = cx + dx; my = cy + dy; }
                }
            { const float c = PatchCost(a, b, x, y, x, y, patch); if (c < best) { best = c; mx = 0; my = 0; } }   // the zero vector is always a candidate: compared last (it never moves the search centre) and without the coarse-vector penalty, so flat areas drop a wrong coarse vector
            float fx = float(mx), fy = float(my);
            if (subpixel) {   // parabola through the plain patch costs at the best integer vector and its four neighbours: a fractional vector
                const float c0 = PatchCost(a, b, x, y, x + mx, y + my, patch);
                const float cxm = PatchCost(a, b, x, y, x + mx - 1, y + my, patch), cxp = PatchCost(a, b, x, y, x + mx + 1, y + my, patch);
                const float cym = PatchCost(a, b, x, y, x + mx, y + my - 1, patch), cyp = PatchCost(a, b, x, y, x + mx, y + my + 1, patch);
                const float dxn = cxm + cxp - 2.0f * c0, dyn = cym + cyp - 2.0f * c0;
                if (c0 <= cxm && c0 <= cxp && dxn > 1e-6f) fx += std::clamp(0.5f * (cxm - cxp) / dxn, -0.5f, 0.5f);
                if (c0 <= cym && c0 <= cyp && dyn > 1e-6f) fy += std::clamp(0.5f * (cym - cyp) / dyn, -0.5f, 0.5f);
            }
            const size_t i = f.idx(x, y);
            f.x[i] = fx; f.y[i] = fy; f.cost[i] = best;
        }
    return f;
}

// Pyramid of luma images: level[0] is level 1 of the pyramid (half resolution), level[3] is 1/16.
struct Pyramid { Img lv[kLevels]; };
inline Pyramid BuildPyramid(const Img& full_luma) {
    Pyramid p;
    p.lv[0] = Down(full_luma);
    for (int i = 1; i < kLevels; ++i) p.lv[i] = Down(p.lv[i - 1]);
    return p;
}

// The whole chain in one direction. `final_index` is the pyramid index of the finished field: 0 = level 1 (half resolution, the finest),
// 1 = level 2 (quarter resolution), ... Stopping earlier skips the most expensive refinement passes and gives a smoother field.
inline Flow Estimate(const Pyramid& a, const Pyramid& b, int final_index = 0, bool subpixel = false) {
    Flow f = Coarse(a.lv[kLevels - 1], b.lv[kLevels - 1]);
    for (int i = kLevels - 2; i >= final_index; --i) f = Refine(a.lv[i], b.lv[i], f, kPatch[i], kRadius[i], subpixel && i == final_index);
    return f;
}

// flow_occlusion.comp: 0 = the forward and backward vectors agree, 1 = they do not (or the target is outside the frame).
inline float Smoothstep(float e0, float e1, float x) { const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
inline std::vector<float> Occlusion(const Flow& fwd, const Flow& bwd) {
    std::vector<float> m(size_t(fwd.w) * fwd.h, 0.0f);
    for (int y = 0; y < fwd.h; ++y)
        for (int x = 0; x < fwd.w; ++x) {
            const size_t i = fwd.idx(x, y);
            const int tx = int(std::floor(float(x) + fwd.x[i] + 0.5f)), ty = int(std::floor(float(y) + fwd.y[i] + 0.5f));
            if (tx < 0 || ty < 0 || tx >= fwd.w || ty >= fwd.h) { m[i] = 1.0f; continue; }
            const size_t j = bwd.idx(tx, ty);
            const float ex = fwd.x[i] + bwd.x[j], ey = fwd.y[i] + bwd.y[j];
            m[i] = Smoothstep(kOcclThreshold, 2.0f * kOcclThreshold, std::sqrt(ex * ex + ey * ey));
        }
    return m;
}


// ---- Stage 5: the interpolation (shaders/flow_interp.comp) ----
constexpr float kFastLow  = 48.0f;    // |v| (full-resolution pixels) where the interpolation starts to fade to the cross-fade
constexpr float kFastHigh = 96.0f;    // ... and where it is only the cross-fade

inline float Bilinear(const Img& s, float x, float y) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = int(fx), iy = int(fy);
    const float tx = x - fx, ty = y - fy;
    return (s.get(ix, iy) * (1 - tx) + s.get(ix + 1, iy) * tx) * (1 - ty) + (s.get(ix, iy + 1) * (1 - tx) + s.get(ix + 1, iy + 1) * tx) * ty;
}

// One output pixel at fractional time t between A (t = 0) and B (t = 1). `fwd` is the flow A -> B in pixels of its own level and `scale` is how
// many full-resolution pixels one of its pixels covers (2 for the level-1 field, 4 for level 2), `mask` the occlusion mask of that field.
// The vector is interpolated bilinearly between the four nearest field pixels; both frames are sampled along it (A at p - t*v, B at
// p + (1-t)*v) and blended by t; occluded or very fast pixels fall back to the cross-fade.
// Colour guidance (flow_interp.comp mirrors this): the four field vectors around a pixel are weighted not only bilinearly but by how
// close the pixel's own luma is to the luma at each field pixel's centre, so a vector does not bleed across a sprite outline.
// Confidence: the match cost stored with the vector fades the result to the cross-fade where the match was poor.
struct InterpOpts {
    bool  guided{true};
    float sigma{0.08f};          // luma difference (0..1) at which a neighbour's weight has fallen to exp(-1/2)
    float floor_w{0.02f};        // weight every neighbour keeps, so the sum never vanishes
    bool  conf{true};
    float cost_lo{0.06f};        // match cost (mean absolute luma difference plus penalties) where the fade to the cross-fade starts
    float cost_hi{0.14f};        // ... and where it is complete
    bool  cand{true};            // candidate search at occluded / badly matched pixels (needs the backward field)
    float cand_trigger{0.05f};   // occlusion or match-cost fade above which the search runs
    float cand_thr{0.06f};       // the best candidate is used only if A and B agree at least this well (luma difference); otherwise cross-fade
    int   cand_radius{2};        // candidates come from the (2r+1)^2 field pixels around the pixel, forward vectors and negated backward vectors
};

inline float InterpPixel(const Img& a, const Img& b, const Flow& fwd, const std::vector<float>& mask, int x, int y, float t, float scale = 2.0f,
                       const InterpOpts& o = InterpOpts{}, const Flow* bwd = nullptr) {
    const float fx = (float(x) + 0.5f) / scale - 0.5f, fy = (float(y) + 0.5f) / scale - 0.5f;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const float tx = fx - float(ix), ty = fy - float(iy);
    const float gp = a.get(x, y);
    float vx = 0.0f, vy = 0.0f, cost = 0.0f, wsum = 0.0f;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            float w = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            const int cxi = std::clamp(ix + i, 0, fwd.w - 1), cyi = std::clamp(iy + j, 0, fwd.h - 1);
            const size_t c = fwd.idx(cxi, cyi);
            if (o.guided) {
                const int gx = int(std::lround((float(ix + i) + 0.5f) * scale - 0.5f)), gy = int(std::lround((float(iy + j) + 0.5f) * scale - 0.5f));
                const float d = a.get(gx, gy) - gp;
                w *= o.floor_w + std::exp(-d * d / (2.0f * o.sigma * o.sigma));
            }
            vx += w * fwd.x[c]; vy += w * fwd.y[c]; cost += w * fwd.cost[c]; wsum += w;
        }
    const float inv = 1.0f / std::max(wsum, 1e-6f);
    vx *= inv * scale; vy *= inv * scale; cost *= inv;                          // full-resolution pixels
    const int qx = std::clamp(int(std::floor(fx + 0.5f)), 0, fwd.w - 1), qy = std::clamp(int(std::floor(fy + 0.5f)), 0, fwd.h - 1);
    const size_t qi = fwd.idx(qx, qy);
    const float fade = (1 - t) * a.get(x, y) + t * b.get(x, y);
    float k = mask[qi];
    if (o.conf) k = std::max(k, Smoothstep(o.cost_lo, o.cost_hi, cost));

    // Occluded or badly matched pixel: the vector of the neighbouring field pixels that makes A and B agree at this pixel's position wins.
    // That is how the strip behind a moving sprite finds the background's vector and the strip in front finds the sprite's.
    if (o.cand && bwd && k > o.cand_trigger) {
        float best = 1e30f, bvx = 0.0f, bvy = 0.0f;
        auto try_vec = [&](float cvx, float cvy, float bias) {
            const float ca = a.get(int(std::lround(float(x) - t * cvx)), int(std::lround(float(y) - t * cvy)));
            const float cb = b.get(int(std::lround(float(x) + (1 - t) * cvx)), int(std::lround(float(y) + (1 - t) * cvy)));
            const float c = std::fabs(ca - cb) + bias;
            if (c < best) { best = c; bvx = cvx; bvy = cvy; }
        };
        try_vec(0.0f, 0.0f, 0.0f);
        for (int dy = -o.cand_radius; dy <= o.cand_radius; ++dy)
            for (int dx = -o.cand_radius; dx <= o.cand_radius; ++dx) {
                const size_t fi = fwd.idx(std::clamp(qx + dx, 0, fwd.w - 1), std::clamp(qy + dy, 0, fwd.h - 1));
                try_vec(fwd.x[fi] * scale, fwd.y[fi] * scale, 0.0005f * (std::fabs(fwd.x[fi] * scale - vx) + std::fabs(fwd.y[fi] * scale - vy)));
                const size_t bi = bwd->idx(std::clamp(qx + dx, 0, bwd->w - 1), std::clamp(qy + dy, 0, bwd->h - 1));
                try_vec(-bwd->x[bi] * scale, -bwd->y[bi] * scale, 0.0005f * (std::fabs(-bwd->x[bi] * scale - vx) + std::fabs(-bwd->y[bi] * scale - vy)));
            }
        if (best < o.cand_thr) {
            const float w2 = (1 - t) * Bilinear(a, float(x) - t * bvx, float(y) - t * bvy) + t * Bilinear(b, float(x) + (1 - t) * bvx, float(y) + (1 - t) * bvy);
            const float sp = Smoothstep(kFastLow, kFastHigh, std::sqrt(bvx * bvx + bvy * bvy));
            return w2 + (fade - w2) * sp;
        }
        return fade;
    }

    const float moved = (1 - t) * Bilinear(a, float(x) - t * vx, float(y) - t * vy) + t * Bilinear(b, float(x) + (1 - t) * vx, float(y) + (1 - t) * vy);
    const float speed = Smoothstep(kFastLow, kFastHigh, std::sqrt(vx * vx + vy * vy));
    k = std::max(k, speed);
    return moved + (fade - moved) * k;
}

} // namespace flowref
