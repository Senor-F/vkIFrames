// The CPU reference of the one-sided fill (tools/interp_eval FillConsistRef, mirrored by apply_fill in shaders/flow_interp.comp) on a synthetic scene with
// exact truth: a square moves over a smooth background. Checks (1) the fill changes only pixels the forward/backward check calls occluded: everywhere else
// the output is BIT-identical to the bidirectional interpolation (the flag-off path), (2) on the occluded pixels it is closer to the truth than the
// bidirectional output (the ideal case; real footage is different, see the README), (3) the output stays finite and in range. No GPU needed.
#define main interp_eval_main
#include "../tools/interp_eval/interp_eval.cpp"
#undef main
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

static void Background(float x, float y, float* rgb) {          // smooth, detailed enough for the flow
    rgb[0] = 0.5f + 0.25f * std::sin(x * 0.05f) * std::cos(y * 0.04f) + 0.15f * std::sin(x * 0.21f + y * 0.13f);
    rgb[1] = 0.45f + 0.30f * std::cos(x * 0.03f + y * 0.06f) + 0.10f * std::sin(y * 0.19f);
    rgb[2] = 0.40f + 0.20f * std::sin((x + y) * 0.045f) + 0.12f * std::cos(x * 0.17f - y * 0.11f);
}
static void Frame(Rgb& img, int w, int h, float sx) {            // the square's left edge at sx
    img.w = w; img.h = h; img.v.assign(size_t(w) * h * 3, 0.0f);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float* p = img.px(x, y); Background(float(x), float(y), p);
        if (x >= sx && x < sx + 56 && y >= 60 && y < 116) { p[0] = 0.9f; p[1] = 0.2f; p[2] = 0.15f; }
    }
}

int main() {
    const int W = 320, H = 192, level = 2;
    Rgb a, b, truth; Frame(a, W, H, 100.0f); Frame(b, W, H, 124.0f); Frame(truth, W, H, 112.0f);
    Ctx c{a, b, Luma(a), Luma(b), {}, {}, {}, {}, 0};
    const Pyramid pa = BuildPyramid(c.la), pb = BuildPyramid(c.lb);
    c.fwd = Estimate(pa, pb, level - 1, true); c.bwd = Estimate(pb, pa, level - 1, true);
    c.mask_f = Occlusion(c.fwd, c.bwd); c.mask_b = Occlusion(c.bwd, c.fwd); c.scale = float(1 << level);
    c.pan_k = Smoothstep(0.15f, 0.35f, FrameBad(c));
    Rgb base; base.w = W; base.h = H; base.v.assign(size_t(W) * H * 3, 0.0f);
    Rgb fill = base;
    Bidirectional(c, 0.5f, base);
    FillConsistRef(c, 0.5f, fill);

    size_t occluded = 0, same = 0, other = 0; double err_base = 0, err_fill = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const int qx = std::clamp(int(std::floor((float(x) + 0.5f) / c.scale)), 0, c.fwd.w - 1), qy = std::clamp(int(std::floor((float(y) + 0.5f) / c.scale)), 0, c.fwd.h - 1);
        const float occ = Smoothstep(kFillOccLo, kFillOccHi, std::max(c.mask_f[c.fwd.idx(qx, qy)], c.mask_b[c.bwd.idx(qx, qy)]));
        for (int ch = 0; ch < 3; ++ch) {
            const float f = fill.px(x, y)[ch], g = base.px(x, y)[ch];
            CHECK(std::isfinite(f) && f > -0.05f && f < 1.05f);
            if (occ <= 0.0f) { CHECK(f == g); }                                   // bit-identical where the check says "not occluded"
        }
        if (occ <= 0.0f) { ++same; continue; }
        ++other;
        if (occ > 0.5f) {
            ++occluded;
            for (int ch = 0; ch < 3; ++ch) { err_base += std::fabs(base.px(x, y)[ch] - truth.px(x, y)[ch]); err_fill += std::fabs(fill.px(x, y)[ch] - truth.px(x, y)[ch]); }
        }
    }
    std::printf("pixels: %zu untouched (bit-identical), %zu with an occlusion weight, %zu of them above 0.5; mean error there: bidirectional %.5f, fill %.5f\n",
                same, other, occluded, occluded ? err_base / (3.0 * double(occluded)) : 0.0, occluded ? err_fill / (3.0 * double(occluded)) : 0.0);
    CHECK(same > size_t(W) * H / 2);                // most of the frame is not occluded
    CHECK(occluded > 200);                          // the scene really has an occlusion
    CHECK(err_fill < err_base);                     // the ideal case: flat-ish background revealed behind the square
    std::printf("PASS: fill reference\n");
    return 0;
}
