// strikers-quest r42: see xr_text.h.

#include "xr_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

#include "xr_font_luckiest.h"

namespace xrtext {
namespace {

struct Color {
  float r, g, b, a;  // straight alpha; sRGB-encoded values, composited as they are (UI art)
};

Color rgb(uint32_t v, float a = 1.0f) {
  return {((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, a};
}
Color mix(Color a, Color b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Premultiplied RGBA, float.
struct Canvas {
  int w, h;
  std::vector<float> px;
  Canvas(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 4, 0.0f) {}
  void over(int x, int y, const Color& c, float cover) {
    if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0.0f) return;
    const float a = c.a * clamp01(cover);
    float* p = &px[(static_cast<size_t>(y) * w + x) * 4];
    p[0] = c.r * a + p[0] * (1.0f - a);
    p[1] = c.g * a + p[1] * (1.0f - a);
    p[2] = c.b * a + p[2] * (1.0f - a);
    p[3] = a + p[3] * (1.0f - a);
  }
  void write(uint8_t* out) const {
    for (size_t i = 0; i < px.size(); ++i) out[i] = static_cast<uint8_t>(std::lround(clamp01(px[i]) * 255.0f));
  }
};

// Set up once, thread-safely (the banner is drawn off the game thread). Only read after that.
stbtt_fontinfo& font() {
  static stbtt_fontinfo f = [] {
    stbtt_fontinfo info;
    stbtt_InitFont(&info, kLuckiestGuy, stbtt_GetFontOffsetForIndex(kLuckiestGuy, 0));
    return info;
  }();
  return f;
}

float text_width(const char* text, float px) {
  stbtt_fontinfo& f = font();
  const float scale = stbtt_ScaleForPixelHeight(&f, px);
  float x = 0.0f;
  for (const char* c = text; *c != '\0'; ++c) {
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f, static_cast<unsigned char>(*c), &advance, &lsb);
    x += advance * scale;
    if (c[1] != '\0') {
      x += scale * stbtt_GetCodepointKernAdvance(&f, static_cast<unsigned char>(c[0]), static_cast<unsigned char>(c[1]));
    }
  }
  return x;
}

// The glyphs' coverage, `pad` pixels of room all round.
struct Mask {
  int w = 0, h = 0;
  std::vector<float> a;
};

Mask text_mask(const char* text, float px, int pad) {
  stbtt_fontinfo& f = font();
  const float scale = stbtt_ScaleForPixelHeight(&f, px);
  int ascent = 0, descent = 0, gap = 0;
  stbtt_GetFontVMetrics(&f, &ascent, &descent, &gap);
  Mask m;
  m.w = static_cast<int>(std::ceil(text_width(text, px))) + 2 * pad;
  m.h = static_cast<int>(std::ceil((ascent - descent) * scale)) + 2 * pad;
  m.a.assign(static_cast<size_t>(m.w) * m.h, 0.0f);
  const int baseline = pad + static_cast<int>(std::lround(ascent * scale));
  float x = static_cast<float>(pad);
  std::vector<unsigned char> glyph;
  for (const char* c = text; *c != '\0'; ++c) {
    const int cp = static_cast<unsigned char>(*c);
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f, cp, &advance, &lsb);
    const float shift = x - std::floor(x);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBoxSubpixel(&f, cp, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0, gh = y1 - y0;
    if (gw > 0 && gh > 0) {
      glyph.assign(static_cast<size_t>(gw) * gh, 0);
      stbtt_MakeCodepointBitmapSubpixel(&f, glyph.data(), gw, gh, gw, scale, scale, shift, 0.0f, cp);
      for (int yy = 0; yy < gh; ++yy) {
        for (int xx = 0; xx < gw; ++xx) {
          const int mx = static_cast<int>(std::floor(x)) + x0 + xx, my = baseline + y0 + yy;
          if (mx < 0 || my < 0 || mx >= m.w || my >= m.h) continue;
          float& d = m.a[static_cast<size_t>(my) * m.w + mx];
          d = std::max(d, glyph[static_cast<size_t>(yy) * gw + xx] / 255.0f);
        }
      }
    }
    x += advance * scale;
    if (c[1] != '\0') x += scale * stbtt_GetCodepointKernAdvance(&f, cp, static_cast<unsigned char>(c[1]));
  }
  return m;
}

// Pixels from each pixel to the shape (0 inside): a two-pass chamfer distance, close enough to round
// for an outline.
std::vector<float> distance_outside(const Mask& m) {
  const float kInf = 1e9f, kA = 1.0f, kB = 1.41421f;
  std::vector<float> d(m.a.size());
  for (size_t i = 0; i < d.size(); ++i) d[i] = m.a[i] >= 0.5f ? 0.0f : kInf;
  auto at = [&](int x, int y) -> float& { return d[static_cast<size_t>(y) * m.w + x]; };
  for (int y = 0; y < m.h; ++y) {
    for (int x = 0; x < m.w; ++x) {
      float v = at(x, y);
      if (x > 0) v = std::min(v, at(x - 1, y) + kA);
      if (y > 0) {
        v = std::min(v, at(x, y - 1) + kA);
        if (x > 0) v = std::min(v, at(x - 1, y - 1) + kB);
        if (x + 1 < m.w) v = std::min(v, at(x + 1, y - 1) + kB);
      }
      at(x, y) = v;
    }
  }
  for (int y = m.h - 1; y >= 0; --y) {
    for (int x = m.w - 1; x >= 0; --x) {
      float v = at(x, y);
      if (x + 1 < m.w) v = std::min(v, at(x + 1, y) + kA);
      if (y + 1 < m.h) {
        v = std::min(v, at(x, y + 1) + kA);
        if (x + 1 < m.w) v = std::min(v, at(x + 1, y + 1) + kB);
        if (x > 0) v = std::min(v, at(x - 1, y + 1) + kB);
      }
      at(x, y) = v;
    }
  }
  return d;
}

struct TextStyle {
  float px;
  Color fillTop, fillBottom;
  Color outline;
  float outlinePx;
  Color shadow;
  float shadowX, shadowY;
};

// `text` centred on (cx, cy): drop shadow, outline, then a top-to-bottom gradient fill.
void draw_text(Canvas& cv, const char* text, float cx, float cy, const TextStyle& s) {
  const int pad = static_cast<int>(std::ceil(s.outlinePx)) + 2;
  const Mask m = text_mask(text, s.px, pad);
  const std::vector<float> dist = distance_outside(m);
  int top = m.h, bottom = 0;
  for (int y = 0; y < m.h; ++y) {
    for (int x = 0; x < m.w; ++x) {
      if (m.a[static_cast<size_t>(y) * m.w + x] > 0.5f) {
        top = std::min(top, y);
        bottom = std::max(bottom, y);
        break;
      }
    }
  }
  const float span = std::max(1.0f, static_cast<float>(bottom - top));
  const int ox = static_cast<int>(std::lround(cx - m.w * 0.5f));
  const int oy = static_cast<int>(std::lround(cy - (top + bottom) * 0.5f));
  auto ring = [&](size_t i) { return std::max(m.a[i], clamp01(s.outlinePx + 0.5f - dist[i])); };
  const int sx = static_cast<int>(std::lround(s.shadowX)), sy = static_cast<int>(std::lround(s.shadowY));
  if (s.shadow.a > 0.0f) {
    for (int y = 0; y < m.h; ++y)
      for (int x = 0; x < m.w; ++x) cv.over(ox + x + sx, oy + y + sy, s.shadow, ring(static_cast<size_t>(y) * m.w + x));
  }
  for (int y = 0; y < m.h; ++y)
    for (int x = 0; x < m.w; ++x) cv.over(ox + x, oy + y, s.outline, ring(static_cast<size_t>(y) * m.w + x));
  for (int y = 0; y < m.h; ++y) {
    const Color c = mix(s.fillTop, s.fillBottom, clamp01((y - top) / span));
    for (int x = 0; x < m.w; ++x) cv.over(ox + x, oy + y, c, m.a[static_cast<size_t>(y) * m.w + x]);
  }
}

// Signed distance to a rounded rectangle (negative inside).
float rounded_rect_sd(float px, float py, float x0, float y0, float x1, float y1, float radius) {
  const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
  const float hx = 0.5f * (x1 - x0) - radius, hy = 0.5f * (y1 - y0) - radius;
  const float qx = std::fabs(px - cx) - hx, qy = std::fabs(py - cy) - hy;
  const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

// A rounded panel: a dark rim, a `border` ring, and a top-to-bottom gradient inside with a soft
// gloss over its upper half.
void panel(Canvas& cv, float x0, float y0, float x1, float y1, float radius, float border, Color rim, Color ring,
           Color top, Color bottom) {
  const float rimPx = 4.0f;
  for (int y = 0; y < cv.h; ++y) {
    const float t = clamp01((y - y0) / std::max(1.0f, y1 - y0));
    const Color fill = mix(top, bottom, t);
    const Color gloss{1.0f, 1.0f, 1.0f, 0.16f * clamp01(1.0f - 2.2f * t)};
    for (int x = 0; x < cv.w; ++x) {
      const float sd = rounded_rect_sd(x + 0.5f, y + 0.5f, x0, y0, x1, y1, radius);
      if (sd > 1.0f) continue;
      cv.over(x, y, rim, 0.5f - sd);
      cv.over(x, y, ring, 0.5f - (sd + rimPx));
      const float inner = 0.5f - (sd + rimPx + border);
      cv.over(x, y, fill, inner);
      cv.over(x, y, gloss, inner);
    }
  }
}

// A five-point star (4x4 samples a pixel), with a dark edge.
void star(Canvas& cv, float cx, float cy, float radius, Color fill, Color edge) {
  auto inside = [&](float px, float py, float r) {
    float vx[10], vy[10];
    for (int i = 0; i < 10; ++i) {
      const float ang = -1.5707963f + i * 0.6283185f;
      const float rr = (i % 2 == 0) ? r : r * 0.45f;
      vx[i] = cx + rr * std::cos(ang);
      vy[i] = cy + rr * std::sin(ang);
    }
    bool in = false;
    for (int i = 0, j = 9; i < 10; j = i++) {
      if (((vy[i] > py) != (vy[j] > py)) && (px < (vx[j] - vx[i]) * (py - vy[i]) / (vy[j] - vy[i]) + vx[i])) in = !in;
    }
    return in;
  };
  const float outer = radius + 6.0f;
  for (int y = static_cast<int>(cy - outer); y <= static_cast<int>(cy + outer); ++y) {
    for (int x = static_cast<int>(cx - outer); x <= static_cast<int>(cx + outer); ++x) {
      int edgeHits = 0, fillHits = 0;
      for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
          const float px = x + (sx + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
          if (inside(px, py, outer)) ++edgeHits;
          if (inside(px, py, radius)) ++fillHits;
        }
      }
      cv.over(x, y, edge, edgeHits / 16.0f);
      cv.over(x, y, mix(fill, rgb(0xFFFFFF), 0.25f * clamp01((cy - y) / radius)), fillHits / 16.0f);
    }
  }
}

}  // namespace

void goal_banner(uint8_t* rgba, int w, int h, const char* name, uint32_t teamRgb, int goals, int assists,
                 bool hatTrick) {
  Canvas cv(w, h);
  const Color team = rgb(teamRgb);
  // r43d: sized off u (r42's banner height, 420 at 1024 wide); the stats line takes the room below.
  const float u = 0.41f * w;
  panel(cv, 10.0f, 10.0f, w - 10.0f, h - 10.0f, 0.13f * u, 0.03f * u, rgb(0x241300), rgb(0xFFD23F),
        mix(team, rgb(0xFFFFFF), 0.18f), mix(team, rgb(0x000000), 0.45f));

  TextStyle header{0.19f * u, rgb(0xFFFFFF), rgb(0xE6E6E6), rgb(0x161616), 0.018f * u, rgb(0x000000, 0.45f),
                   0.010f * u, 0.012f * u};
  draw_text(cv, "GOAL SCORED BY:", 0.5f * w, 0.29f * u, header);

  // The name as big as it fits between the stars.
  const float starR = 0.12f * u, room = w - 2.0f * (0.06f * w + 2.6f * starR);
  float px = 0.40f * u;
  const float wide = text_width(name, px);
  if (wide > room) px *= room / wide;
  TextStyle big{px, rgb(0xFFF7B0), rgb(0xFF9800), rgb(0x3A1800), 0.026f * u, rgb(0x000000, 0.55f), 0.016f * u,
                0.02f * u};
  draw_text(cv, name, 0.5f * w, 0.65f * u, big);
  star(cv, 0.06f * w + 1.3f * starR, 0.65f * u, starR, rgb(0xFFD23F), rgb(0x3A1800));
  star(cv, w - (0.06f * w + 1.3f * starR), 0.65f * u, starR, rgb(0xFFD23F), rgb(0x3A1800));

  // r43d (Lance: "show their game stats for that game"): goals and assists so far this game.
  if (goals >= 0) {
    char line[96];
    std::snprintf(line, sizeof(line), "%s%d GOAL%s,  %d ASSIST%s", hatTrick ? "HAT TRICK!   " : "THIS GAME:  ", goals,
                  goals == 1 ? "" : "S", assists, assists == 1 ? "" : "S");
    float spx = 0.15f * u;
    const float swide = text_width(line, spx), sroom = w - 0.12f * w;
    if (swide > sroom) spx *= sroom / swide;
    const TextStyle stats = hatTrick ? TextStyle{spx, rgb(0xFFF7B0), rgb(0xFFC21F), rgb(0x3A1800), 0.02f * u,
                                                 rgb(0x000000, 0.5f), 0.01f * u, 0.012f * u}
                                     : TextStyle{spx, rgb(0xFFFFFF), rgb(0xDADADA), rgb(0x161616), 0.016f * u,
                                                 rgb(0x000000, 0.45f), 0.008f * u, 0.01f * u};
    draw_text(cv, line, 0.5f * w, h - 0.24f * u, stats);
  }
  cv.write(rgba);
}

// r44g (r42's crowd volume panel, r44's voice line): the pause settings, one row picked at a time.
void settings_panel(uint8_t* rgba, int w, int h, int level, int steps, int voice, int heightCm, int row) {
  Canvas cv(w, h);
  const float u = 0.3125f * w;  // r42's whole panel height: 200 at 640 wide
  panel(cv, 8.0f, 8.0f, w - 8.0f, h - 8.0f, 0.16f * u, 0.02f * u, rgb(0x000000, 0.9f), rgb(0xFFFFFF, 0.9f),
        rgb(0x1E2A44, 0.92f), rgb(0x0B1020, 0.92f));
  // The rows' bands, top to bottom: crowd volume (title and bar), voice chat, stadium height; the hint.
  const float band[3][2] = {{0.06f * u, 0.76f * u}, {0.80f * u, 1.18f * u}, {1.22f * u, 1.60f * u}};
  const float bx0 = 0.04f * w, bx1 = 0.96f * w;
  if (row >= 0 && row < 3) {  // the picked row: a lighter band with a gold edge
    const float y0 = band[row][0], y1 = band[row][1];
    for (int y = static_cast<int>(y0) - 3; y <= static_cast<int>(y1) + 3; ++y) {
      for (int x = static_cast<int>(bx0) - 3; x <= static_cast<int>(bx1) + 3; ++x) {
        const float sd = rounded_rect_sd(x + 0.5f, y + 0.5f, bx0, y0, bx1, y1, 0.08f * u);
        cv.over(x, y, rgb(0xFFD23F, 0.95f), 0.5f - sd);
        cv.over(x, y, rgb(0x34507A, 0.95f), 0.5f - (sd + 3.0f));
      }
    }
  }
  auto line_style = [&](float px, bool picked, uint32_t top, uint32_t bottom) {
    return TextStyle{px, rgb(picked ? 0xFFF7B0 : top), rgb(picked ? 0xFFC21F : bottom), rgb(0x05070C), 0.016f * u,
                     rgb(0x000000, 0.0f), 0.0f, 0.0f};
  };
  auto fitted = [&](const char* text, float px) {
    const float wide = text_width(text, px), room = 0.86f * w;
    return wide > room ? px * room / wide : px;
  };
  // Crowd volume.
  draw_text(cv, "CROWD VOLUME", 0.5f * w, 0.22f * u, line_style(0.22f * u, row == 0, 0xFFFFFF, 0xDDE4F0));
  const float x0 = 0.08f * w, x1 = 0.92f * w, gap = 0.012f * w;
  const float segW = ((x1 - x0) - gap * (steps - 1)) / steps;
  const float y0 = 0.42f * u, y1 = 0.64f * u;
  for (int i = 0; i < steps; ++i) {
    const float sx0 = x0 + i * (segW + gap), sx1 = sx0 + segW;
    const bool lit = i < level;
    for (int y = static_cast<int>(y0) - 2; y <= static_cast<int>(y1) + 2; ++y) {
      const float t = clamp01((y - y0) / (y1 - y0));
      const Color c = lit ? mix(rgb(0xFFF07A), rgb(0xFF9800), t) : mix(rgb(0x48526A), rgb(0x2A3142), t);
      for (int x = static_cast<int>(sx0) - 2; x <= static_cast<int>(sx1) + 2; ++x) {
        cv.over(x, y, c, 0.5f - rounded_rect_sd(x + 0.5f, y + 0.5f, sx0, y0, sx1, y1, 0.2f * segW));
      }
    }
  }
  // Voice chat: on (green), off (grey), or on without the microphone allowed (amber).
  const char* v = voice == 1 ? "VOICE CHAT: ON" : (voice == 2 ? "VOICE CHAT: ON, NO MIC ACCESS" : "VOICE CHAT: OFF");
  const uint32_t vTop = voice == 1 ? 0x9CFF8A : (voice == 2 ? 0xFFD27A : 0xB8C2D6);
  const uint32_t vBottom = voice == 1 ? 0x3CC23C : (voice == 2 ? 0xFF9800 : 0x9AA6BE);
  draw_text(cv, v, 0.5f * w, 0.99f * u, line_style(fitted(v, 0.18f * u), row == 1, vTop, vBottom));
  // Your eye height in the life-size stadium (r44g).
  char hl[64];
  if (heightCm == 0) {
    std::snprintf(hl, sizeof(hl), "STADIUM HEIGHT: NORMAL");
  } else {
    std::snprintf(hl, sizeof(hl), "STADIUM HEIGHT: YOU %d CM %s", heightCm > 0 ? heightCm : -heightCm,
                  heightCm > 0 ? "HIGHER" : "LOWER");
  }
  draw_text(cv, hl, 0.5f * w, 1.41f * u, line_style(fitted(hl, 0.18f * u), row == 2, 0xFFFFFF, 0xDDE4F0));
  const char* hint = "RIGHT STICK:  UP / DOWN PICKS,  LEFT / RIGHT CHANGES";
  TextStyle hs{fitted(hint, 0.12f * u), rgb(0xB8C2D6), rgb(0x9AA6BE), rgb(0x05070C), 0.012f * u, rgb(0x000000, 0.0f),
               0.0f, 0.0f};
  draw_text(cv, hint, 0.5f * w, 0.5f * (1.60f * u + h - 8.0f), hs);
  cv.write(rgba);
}

}  // namespace xrtext
