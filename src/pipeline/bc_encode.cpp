#include "pipeline/bc_encode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace wowee::pipeline {

namespace {

struct Rgb {
    float r = 0, g = 0, b = 0;
};

uint16_t pack565(const Rgb& c) {
    const int r = std::clamp(static_cast<int>(std::lround(c.r * 31.0f / 255.0f)), 0, 31);
    const int g = std::clamp(static_cast<int>(std::lround(c.g * 63.0f / 255.0f)), 0, 63);
    const int b = std::clamp(static_cast<int>(std::lround(c.b * 31.0f / 255.0f)), 0, 31);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

std::array<int, 3> unpack565(uint16_t v) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

/// The four colours a BC1 block in four-colour mode decodes to.
std::array<std::array<int, 3>, 4> palette(uint16_t c0, uint16_t c1) {
    const auto a = unpack565(c0), b = unpack565(c1);
    std::array<std::array<int, 3>, 4> p{};
    for (int k = 0; k < 3; ++k) {
        p[0][k] = a[k];
        p[1][k] = b[k];
        p[2][k] = (2 * a[k] + b[k]) / 3;
        p[3][k] = (a[k] + 2 * b[k]) / 3;
    }
    return p;
}

/// Picks each texel's nearest colour; returns the squared error.
long indexColours(const std::array<std::array<int, 3>, 16>& px, uint16_t c0, uint16_t c1,
                  std::array<uint8_t, 16>& idx) {
    const auto p = palette(c0, c1);
    long total = 0;
    for (int i = 0; i < 16; ++i) {
        long best = -1;
        for (int j = 0; j < 4; ++j) {
            const long dr = px[i][0] - p[j][0], dg = px[i][1] - p[j][1], db = px[i][2] - p[j][2];
            const long e = dr * dr + dg * dg + db * db;
            if (best < 0 || e < best) {
                best = e;
                idx[i] = static_cast<uint8_t>(j);
            }
        }
        total += best;
    }
    return total;
}

/// A BC1 colour block, always in four-colour mode (c0 > c1), as BC3's
/// colour half must be.
void encodeColourBlock(const std::array<std::array<int, 3>, 16>& px, uint8_t* out) {
    // Mean and covariance, then the principal axis by power iteration.
    Rgb mean;
    for (const auto& p : px) {
        mean.r += p[0];
        mean.g += p[1];
        mean.b += p[2];
    }
    mean.r /= 16.0f;
    mean.g /= 16.0f;
    mean.b /= 16.0f;
    float cov[6] = {0, 0, 0, 0, 0, 0};  // rr rg rb gg gb bb
    for (const auto& p : px) {
        const float r = p[0] - mean.r, g = p[1] - mean.g, b = p[2] - mean.b;
        cov[0] += r * r;
        cov[1] += r * g;
        cov[2] += r * b;
        cov[3] += g * g;
        cov[4] += g * b;
        cov[5] += b * b;
    }
    Rgb axis{1.0f, 1.0f, 1.0f};
    for (int it = 0; it < 8; ++it) {
        Rgb n{cov[0] * axis.r + cov[1] * axis.g + cov[2] * axis.b,
              cov[1] * axis.r + cov[3] * axis.g + cov[4] * axis.b,
              cov[2] * axis.r + cov[4] * axis.g + cov[5] * axis.b};
        const float len = std::sqrt(n.r * n.r + n.g * n.g + n.b * n.b);
        if (len < 1e-6f) break;
        axis = {n.r / len, n.g / len, n.b / len};
    }
    float lo = 0.0f, hi = 0.0f;
    for (const auto& p : px) {
        const float t = (p[0] - mean.r) * axis.r + (p[1] - mean.g) * axis.g + (p[2] - mean.b) * axis.b;
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    Rgb e0{mean.r + axis.r * hi, mean.g + axis.g * hi, mean.b + axis.b * hi};
    Rgb e1{mean.r + axis.r * lo, mean.g + axis.g * lo, mean.b + axis.b * lo};

    uint16_t c0 = pack565(e0), c1 = pack565(e1);
    std::array<uint8_t, 16> idx{};
    if (c0 < c1) std::swap(c0, c1);
    long err = indexColours(px, c0, c1, idx);

    // One least-squares pass: the ends that fit the chosen indices best.
    if (c0 != c1) {
        static constexpr float kW[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
        float aa = 0, bb = 0, ab = 0;
        Rgb ax, bx;
        for (int i = 0; i < 16; ++i) {
            const float a = kW[idx[i]], b = 1.0f - a;
            aa += a * a;
            bb += b * b;
            ab += a * b;
            ax.r += a * px[i][0];
            ax.g += a * px[i][1];
            ax.b += a * px[i][2];
            bx.r += b * px[i][0];
            bx.g += b * px[i][1];
            bx.b += b * px[i][2];
        }
        const float det = aa * bb - ab * ab;
        if (std::abs(det) > 1e-6f) {
            const float f = 1.0f / det;
            Rgb n0{(ax.r * bb - bx.r * ab) * f, (ax.g * bb - bx.g * ab) * f, (ax.b * bb - bx.b * ab) * f};
            Rgb n1{(bx.r * aa - ax.r * ab) * f, (bx.g * aa - ax.g * ab) * f, (bx.b * aa - ax.b * ab) * f};
            uint16_t d0 = pack565(n0), d1 = pack565(n1);
            if (d0 < d1) std::swap(d0, d1);
            if (d0 != d1) {
                std::array<uint8_t, 16> idx2{};
                const long err2 = indexColours(px, d0, d1, idx2);
                if (err2 < err) {
                    c0 = d0;
                    c1 = d1;
                    idx = idx2;
                    err = err2;
                }
            }
        }
    }
    if (c0 == c1) idx.fill(0);  // one colour: the block's three-colour mode reads index 0 the same

    out[0] = static_cast<uint8_t>(c0 & 0xFF);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1 & 0xFF);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= static_cast<uint32_t>(idx[i]) << (2 * i);
    std::memcpy(out + 4, &bits, 4);  // little-endian, as every target is
}

/// BC3's alpha half: two ends and eight steps between them (a0 > a1).
void encodeAlphaBlock(const std::array<int, 16>& alpha, uint8_t* out) {
    int lo = 255, hi = 0;
    for (int a : alpha) {
        lo = std::min(lo, a);
        hi = std::max(hi, a);
    }
    out[0] = static_cast<uint8_t>(hi);
    out[1] = static_cast<uint8_t>(lo);
    uint64_t bits = 0;
    if (hi > lo) {
        int values[8];
        values[0] = hi;
        values[1] = lo;
        for (int k = 1; k <= 6; ++k) values[k + 1] = ((7 - k) * hi + k * lo) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 0, bestErr = 1 << 30;
            for (int j = 0; j < 8; ++j) {
                const int e = std::abs(alpha[i] - values[j]);
                if (e < bestErr) {
                    bestErr = e;
                    best = j;
                }
            }
            bits |= static_cast<uint64_t>(best) << (3 * i);
        }
    }
    for (int k = 0; k < 6; ++k) out[2 + k] = static_cast<uint8_t>(bits >> (8 * k));
}

}  // namespace

std::vector<uint8_t> encodeBC(const uint8_t* rgba, int width, int height, bool withAlpha) {
    if (!rgba || width <= 0 || height <= 0) return {};
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    const size_t blockBytes = withAlpha ? 16 : 8;
    std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * blockBytes);
    std::array<std::array<int, 3>, 16> px{};
    std::array<int, 16> alpha{};
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    // A block past the edge repeats the edge texel.
                    const int sx = std::min(bx * 4 + x, width - 1);
                    const int sy = std::min(by * 4 + y, height - 1);
                    const uint8_t* p = rgba + (static_cast<size_t>(sy) * width + sx) * 4;
                    px[y * 4 + x] = {p[0], p[1], p[2]};
                    alpha[y * 4 + x] = p[3];
                }
            }
            uint8_t* dst = out.data() + (static_cast<size_t>(by) * bw + bx) * blockBytes;
            if (withAlpha) {
                encodeAlphaBlock(alpha, dst);
                encodeColourBlock(px, dst + 8);
            } else {
                encodeColourBlock(px, dst);
            }
        }
    }
    return out;
}

BCLevels encodeBCWithMips(const uint8_t* rgba, int width, int height, bool withAlpha) {
    BCLevels levels;
    levels.alpha = withAlpha;
    if (!rgba || width <= 0 || height <= 0) return levels;
    std::vector<uint8_t> current(rgba, rgba + static_cast<size_t>(width) * height * 4);
    int w = width, h = height;
    for (;;) {
        levels.mips.push_back(encodeBC(current.data(), w, h, withAlpha));
        if (w == 1 && h == 1) break;
        // A 2x2 box down, as the GPU's blit chain did for these textures.
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0, n = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                            sum += current[(static_cast<size_t>(sy) * w + sx) * 4 + c];
                            ++n;
                        }
                    }
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + n / 2) / n);
                }
            }
        }
        current.swap(next);
        w = nw;
        h = nh;
    }
    return levels;
}

}  // namespace wowee::pipeline
