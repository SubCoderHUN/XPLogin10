#include "xplogin/ui/win32/XpRenderer.h"

#include "xplogin/Logging.h"

#include "xplogin/assets/AssetPack.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <map>

namespace xplogin::ui::win32 {
namespace {

// Both Gdiplus and xplogin::ui declare Rect and Point. Unqualified names here
// would resolve to the xplogin::ui ones (enclosing namespace beats a global
// using-directive), which compiles in some places and silently means the wrong
// thing in others. Alias both explicitly and never write either bare.
using UiRect  = xplogin::ui::Rect;
using GpRect  = Gdiplus::Rect;
using GpRectF = Gdiplus::RectF;
using GpPoint = Gdiplus::Point;

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::GraphicsState;
using Gdiplus::LinearGradientBrush;
using Gdiplus::Matrix;
using Gdiplus::PathGradientBrush;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::SolidBrush;
using Gdiplus::StringFormat;
using Gdiplus::REAL;

Color ToColor(Argb argb) {
    return Color(AlphaOf(argb), RedOf(argb), GreenOf(argb), BlueOf(argb));
}

GpRect ToGdiRect(const UiRect& r) {
    return GpRect(r.x, r.y, r.width, r.height);
}

GpRectF ToGdiRectF(const UiRect& r) {
    return GpRectF(static_cast<REAL>(r.x), static_cast<REAL>(r.y),
                   static_cast<REAL>(r.width), static_cast<REAL>(r.height));
}

// Draws a bitmap stretched to fill `bounds`. XP's dividers are 800px sweeps
// stretched across whatever the screen is, so this is the common case.
void StretchImage(Graphics& graphics, Bitmap* bitmap, const UiRect& bounds) {
    if (!bitmap || bounds.IsEmpty()) {
        return;
    }
    // Clamp the sampling to the source edges, or the stretch picks up whatever
    // is adjacent in the texture and fringes the ends of the sweep.
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    graphics.DrawImage(bitmap, ToGdiRect(bounds), 0, 0,
                       static_cast<INT>(bitmap->GetWidth()),
                       static_cast<INT>(bitmap->GetHeight()), Gdiplus::UnitPixel,
                       &attributes);
}

// Draws one frame of a vertical filmstrip, scaled to fill `bounds`.
//
// msgina.dll keeps the shutdown orbs as a single 32x320 bitmap: ten 32x32
// frames stacked, three states for each of the three buttons plus a disabled
// Stand By. Sampling a sub-rectangle of a bitmap is not the same as drawing a
// cropped copy of it - the clamp matters, or a scaled frame picks up a row of
// the frame above and below it and the orbs come out with seams.
void DrawStripFrame(Graphics& graphics, Bitmap* strip, int frameSize,
                    int frameIndex, const UiRect& bounds) {
    if (!strip || bounds.IsEmpty() || frameSize <= 0 || frameIndex < 0) {
        return;
    }
    const int top = frameIndex * frameSize;
    if (top + frameSize > static_cast<int>(strip->GetHeight())) {
        return;
    }
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    graphics.DrawImage(strip, ToGdiRect(bounds), 0, top,
                       static_cast<INT>(strip->GetWidth()), frameSize,
                       Gdiplus::UnitPixel, &attributes);
}

// Fades a colour by a 0..1 factor, for drawing a dimmed tile's text.
Color Fade(Argb argb, float opacity) {
    const float alpha = static_cast<float>(AlphaOf(argb)) *
                        std::clamp(opacity, 0.0f, 1.0f);
    return Color(static_cast<BYTE>(alpha + 0.5f), RedOf(argb), GreenOf(argb),
                 BlueOf(argb));
}

// An ImageAttributes that scales a bitmap's alpha, so a dimmed tile's artwork
// fades with its text instead of staying solid on a faded row.
bool MakeFadeAttributes(float opacity, Gdiplus::ImageAttributes* out) {
    if (opacity >= 0.999f) {
        return false;
    }
    Gdiplus::ColorMatrix matrix = {};
    matrix.m[0][0] = 1.0f;
    matrix.m[1][1] = 1.0f;
    matrix.m[2][2] = 1.0f;
    matrix.m[3][3] = std::clamp(opacity, 0.0f, 1.0f);
    matrix.m[4][4] = 1.0f;
    out->SetColorMatrix(&matrix);
    return true;
}

// Draws a bitmap into `bounds`, optionally faded.
void DrawImageFaded(Graphics& graphics, Bitmap* bitmap, const UiRect& bounds,
                    float opacity) {
    if (!bitmap || bounds.IsEmpty()) {
        return;
    }
    Gdiplus::ImageAttributes attributes;
    if (!MakeFadeAttributes(opacity, &attributes)) {
        graphics.DrawImage(bitmap, ToGdiRect(bounds));
        return;
    }
    graphics.DrawImage(bitmap, ToGdiRect(bounds), 0, 0,
                       static_cast<INT>(bitmap->GetWidth()),
                       static_cast<INT>(bitmap->GetHeight()), Gdiplus::UnitPixel,
                       &attributes);
}

// The margins of a DirectUI `borderthickness`, in source pixels: left, top,
// right, bottom. Corners are drawn 1:1, edges stretch along one axis and the
// middle stretches both ways - which is what stops XP's password field from
// smearing its rounded ends when it is wider than the 171px it ships at.
struct Slice {
    int left, top, right, bottom;
};

void DrawNineSlice(Graphics& graphics, Bitmap* bitmap, const UiRect& bounds,
                   const Slice& margin, float scale, float opacity = 1.0f) {
    if (!bitmap || bounds.IsEmpty()) {
        return;
    }
    const int sw = static_cast<int>(bitmap->GetWidth());
    const int sh = static_cast<int>(bitmap->GetHeight());

    // Destination margins scale with the screen, but never past the point where
    // opposite corners would overlap - at which point the slice degenerates and
    // a plain stretch is the honest answer.
    const int dl = (std::min)(static_cast<int>(margin.left * scale), bounds.width / 2);
    const int dr = (std::min)(static_cast<int>(margin.right * scale), bounds.width / 2);
    const int dt = (std::min)(static_cast<int>(margin.top * scale), bounds.height / 2);
    const int db = (std::min)(static_cast<int>(margin.bottom * scale), bounds.height / 2);

    if (margin.left + margin.right >= sw || margin.top + margin.bottom >= sh) {
        DrawImageFaded(graphics, bitmap, bounds, opacity);
        return;
    }

    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    MakeFadeAttributes(opacity, &attributes);

    // Column and row edges, in source and destination order.
    const int sx[4] = {0, margin.left, sw - margin.right, sw};
    const int sy[4] = {0, margin.top, sh - margin.bottom, sh};
    const int dx[4] = {bounds.x, bounds.x + dl, bounds.Right() - dr, bounds.Right()};
    const int dy[4] = {bounds.y, bounds.y + dt, bounds.Bottom() - db, bounds.Bottom()};

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int dw = dx[col + 1] - dx[col];
            const int dh = dy[row + 1] - dy[row];
            const int srcW = sx[col + 1] - sx[col];
            const int srcH = sy[row + 1] - sy[row];
            if (dw <= 0 || dh <= 0 || srcW <= 0 || srcH <= 0) {
                continue;
            }
            graphics.DrawImage(bitmap, GpRect(dx[col], dy[row], dw, dh), sx[col],
                               sy[row], srcW, srcH, Gdiplus::UnitPixel, &attributes);
        }
    }
}

// Draws a bitmap at its natural size, centred in `bounds`.
void CenterImage(Graphics& graphics, Bitmap* bitmap, const UiRect& bounds,
                 float scale) {
    if (!bitmap || bounds.IsEmpty()) {
        return;
    }
    const int width = static_cast<int>(bitmap->GetWidth() * scale);
    const int height = static_cast<int>(bitmap->GetHeight() * scale);
    const UiRect target{bounds.x + (bounds.width - width) / 2,
                        bounds.y + (bounds.height - height) / 2, width, height};
    graphics.DrawImage(bitmap, ToGdiRect(target));
}

// Fills a rectangle with a multi-stop vertical gradient.
void FillGradient(Graphics& graphics, const UiRect& bounds,
                  const Gradient& gradient) {
    if (bounds.IsEmpty() || gradient.empty()) {
        return;
    }
    const auto& stops = gradient.Stops();
    if (stops.size() == 1) {
        SolidBrush brush(ToColor(stops.front().color));
        graphics.FillRectangle(&brush, ToGdiRect(bounds));
        return;
    }

    LinearGradientBrush brush(GpPoint(bounds.x, bounds.y),
                              GpPoint(bounds.x, bounds.Bottom()),
                              ToColor(stops.front().color),
                              ToColor(stops.back().color));

    std::vector<Color> colors;
    std::vector<REAL> positions;
    colors.reserve(stops.size());
    positions.reserve(stops.size());
    for (const GradientStop& stop : stops) {
        colors.push_back(ToColor(stop.color));
        positions.push_back(std::clamp(stop.position, 0.0f, 1.0f));
    }
    // GDI+ requires the first and last positions to be exactly 0 and 1.
    positions.front() = 0.0f;
    positions.back() = 1.0f;
    brush.SetInterpolationColors(colors.data(), positions.data(),
                                 static_cast<INT>(colors.size()));

    graphics.FillRectangle(&brush, ToGdiRect(bounds));
}

// The soft white rule XP drew where the bands meet: a bright core with a glow
// fading away above or below it.
void DrawGlowLine(Graphics& graphics, const UiRect& line, Argb core, Argb glow,
                  bool glowUp) {
    if (line.IsEmpty()) {
        return;
    }

    const int glowHeight = (std::max)(6, line.height * 6);
    const UiRect glowRect{line.x, glowUp ? line.y - glowHeight : line.Bottom(),
                          line.width, glowHeight};

    LinearGradientBrush glowBrush(
        GpPoint(glowRect.x, glowUp ? glowRect.Bottom() : glowRect.y),
        GpPoint(glowRect.x, glowUp ? glowRect.y : glowRect.Bottom()), ToColor(glow),
        Color(0, RedOf(glow), GreenOf(glow), BlueOf(glow)));
    graphics.FillRectangle(&glowBrush, ToGdiRect(glowRect));

    SolidBrush coreBrush(ToColor(core));
    graphics.FillRectangle(&coreBrush, ToGdiRect(line));
}

// A round XP-style button: dark rim, colour body, specular highlight.
void DrawOrb(Graphics& graphics, const UiRect& bounds, Argb outer, Argb inner,
             bool pressed, bool hovered) {
    if (bounds.IsEmpty()) {
        return;
    }
    const GpRect rect = ToGdiRect(bounds);

    GraphicsPath path;
    path.AddEllipse(rect);
    PathGradientBrush body(&path);
    body.SetCenterColor(ToColor(hovered ? LerpArgb(inner, 0xFFFFFFFFu, 0.25f) : inner));
    Color surround[] = {ToColor(outer)};
    int surroundCount = 1;
    body.SetSurroundColors(surround, &surroundCount);
    // Push the highlight up and left, the way a light source above would.
    body.SetCenterPoint(PointF(rect.X + rect.Width * 0.38f,
                               rect.Y + rect.Height * 0.32f));
    graphics.FillEllipse(&body, rect);

    Pen rim(ToColor(LerpArgb(outer, 0xFF000000u, 0.35f)),
            (std::max)(1.0f, bounds.width / 16.0f));
    graphics.DrawEllipse(&rim, rect);

    if (!pressed) {
        // Specular blob across the top third.
        const GpRect gloss(rect.X + rect.Width / 5, rect.Y + rect.Height / 8,
                           rect.Width * 3 / 5, rect.Height / 3);
        GraphicsPath glossPath;
        glossPath.AddEllipse(gloss);
        PathGradientBrush glossBrush(&glossPath);
        glossBrush.SetCenterColor(Color(150, 255, 255, 255));
        Color glossEdge[] = {Color(0, 255, 255, 255)};
        int glossCount = 1;
        glossBrush.SetSurroundColors(glossEdge, &glossCount);
        graphics.FillEllipse(&glossBrush, gloss);
    }
}

// The IEC power glyph: a broken ring with a vertical bar.
void DrawPowerGlyph(Graphics& graphics, const UiRect& bounds) {
    const REAL inset = bounds.width * 0.28f;
    const GpRectF ring(bounds.x + inset, bounds.y + inset, bounds.width - 2 * inset,
                       bounds.height - 2 * inset);
    Pen pen(Color(230, 255, 255, 255), (std::max)(1.5f, bounds.width / 12.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawArc(&pen, ring, -60.0f, 300.0f);
    graphics.DrawLine(&pen, ring.X + ring.Width / 2,
                      bounds.y + bounds.height * 0.18f, ring.X + ring.Width / 2,
                      ring.Y + ring.Height * 0.45f);
}

void DrawRestartGlyph(Graphics& graphics, const UiRect& bounds) {
    const REAL inset = bounds.width * 0.26f;
    const GpRectF ring(bounds.x + inset, bounds.y + inset, bounds.width - 2 * inset,
                       bounds.height - 2 * inset);
    Pen pen(Color(230, 255, 255, 255), (std::max)(1.5f, bounds.width / 12.0f));
    pen.SetEndCap(Gdiplus::LineCapArrowAnchor);
    graphics.DrawArc(&pen, ring, 30.0f, 300.0f);
}

void DrawStandByGlyph(Graphics& graphics, const UiRect& bounds) {
    // A crescent moon, as on XP's Stand By button.
    const REAL size = bounds.width * 0.5f;
    const REAL cx = bounds.x + bounds.width * 0.5f;
    const REAL cy = bounds.y + bounds.height * 0.5f;

    GraphicsPath moon;
    moon.AddArc(cx - size / 2, cy - size / 2, size, size, 110.0f, 250.0f);
    moon.AddArc(cx - size / 4, cy - size / 2, size, size, 0.0f, -250.0f);
    SolidBrush brush(Color(230, 255, 255, 255));
    graphics.FillPath(&brush, &moon);
}

// The four-pane window mark. Drawn from primitives, not copied from
// Microsoft's bitmap; see NOTICE.md.
void DrawWindowMark(Graphics& graphics, const UiRect& bounds) {
    if (bounds.IsEmpty()) {
        return;
    }
    const Argb paneColors[4] = {0xFFE8453Cu, 0xFF7CBB42u, 0xFF3C8FE8u, 0xFFF6C518u};
    const int gap = (std::max)(2, bounds.width / 14);
    const int paneW = (bounds.width - gap) / 2;
    const int paneH = (bounds.height - gap) / 2;

    const GraphicsState saved = graphics.Save();
    // A slight shear gives the wave XP's flag had.
    Matrix skew(1.0f, 0.0f, -0.18f, 1.0f, bounds.height * 0.18f, 0.0f);
    graphics.MultiplyTransform(&skew);

    for (int i = 0; i < 4; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        const UiRect pane{bounds.x + col * (paneW + gap),
                          bounds.y + row * (paneH + gap), paneW, paneH};
        LinearGradientBrush brush(GpPoint(pane.x, pane.y),
                                  GpPoint(pane.x, pane.Bottom()),
                                  ToColor(LerpArgb(paneColors[i], 0xFFFFFFFFu, 0.25f)),
                                  ToColor(paneColors[i]));
        graphics.FillRectangle(&brush, ToGdiRect(pane));
    }
    graphics.Restore(saved);
}

// ---------------------------------------------------------------------------
// GDI+ process lifetime
// ---------------------------------------------------------------------------

ULONG_PTR g_gdiplusToken = 0;
int g_gdiplusRefCount = 0;

CRITICAL_SECTION* GdiPlusLock() {
    static CRITICAL_SECTION lock;
    static const bool initialized = [] {
        ::InitializeCriticalSection(&lock);
        return true;
    }();
    (void)initialized;
    return &lock;
}

} // namespace

GdiPlusHost::GdiPlusHost() {
    ::EnterCriticalSection(GdiPlusLock());
    if (g_gdiplusRefCount == 0) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok) {
            ready_ = true;
            ++g_gdiplusRefCount;
        }
    } else {
        ++g_gdiplusRefCount;
        ready_ = true;
    }
    ::LeaveCriticalSection(GdiPlusLock());

    if (!ready_) {
        XPLOG_ERROR("GdiplusStartup failed; the welcome screen cannot be drawn");
    }
}

GdiPlusHost::~GdiPlusHost() {
    if (!ready_) {
        return;
    }
    ::EnterCriticalSection(GdiPlusLock());
    if (--g_gdiplusRefCount == 0) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    ::LeaveCriticalSection(GdiPlusLock());
}

// ---------------------------------------------------------------------------
// XpRenderer
// ---------------------------------------------------------------------------

struct XpRenderer::Impl {
    std::vector<RenderUser> users;
    XpAssets assets;
    std::map<std::wstring, std::unique_ptr<Bitmap>> avatarCache;
    std::unique_ptr<FontFamily> headingFamily;
    std::unique_ptr<FontFamily> bodyFamily;
    std::unique_ptr<FontFamily> statusFamily;

    // Back buffer, resized on demand.
    HDC     memoryDc = nullptr;
    HBITMAP memoryBitmap = nullptr;
    HBITMAP previousBitmap = nullptr;
    int     bufferWidth = 0;
    int     bufferHeight = 0;

    ~Impl() { ReleaseBuffer(); }

    void ReleaseBuffer() {
        if (memoryDc) {
            if (previousBitmap) {
                ::SelectObject(memoryDc, previousBitmap);
                previousBitmap = nullptr;
            }
            ::DeleteDC(memoryDc);
            memoryDc = nullptr;
        }
        if (memoryBitmap) {
            ::DeleteObject(memoryBitmap);
            memoryBitmap = nullptr;
        }
        bufferWidth = 0;
        bufferHeight = 0;
    }

    bool EnsureBuffer(HDC target, int width, int height) {
        if (memoryDc && bufferWidth == width && bufferHeight == height) {
            return true;
        }
        ReleaseBuffer();
        if (width <= 0 || height <= 0) {
            return false;
        }
        memoryDc = ::CreateCompatibleDC(target);
        if (!memoryDc) {
            return false;
        }
        memoryBitmap = ::CreateCompatibleBitmap(target, width, height);
        if (!memoryBitmap) {
            ReleaseBuffer();
            return false;
        }
        previousBitmap =
            static_cast<HBITMAP>(::SelectObject(memoryDc, memoryBitmap));
        bufferWidth = width;
        bufferHeight = height;
        return true;
    }

    // Resolves a font family, walking the theme's fallback chain. GDI+ silently
    // substitutes an unrelated face if the family is missing, so check.
    std::unique_ptr<FontFamily> Resolve(const std::wstring& primary,
                                        const std::wstring& fallback) {
        auto family = std::make_unique<FontFamily>(primary.c_str());
        if (family->IsAvailable()) {
            return family;
        }
        family = std::make_unique<FontFamily>(fallback.c_str());
        if (family->IsAvailable()) {
            return family;
        }
        return std::make_unique<FontFamily>(L"Tahoma");
    }

    Bitmap* Avatar(const std::wstring& path) {
        if (path.empty()) {
            return nullptr;
        }
        auto it = avatarCache.find(path);
        if (it != avatarCache.end()) {
            return it->second.get();
        }
        auto bitmap = std::make_unique<Bitmap>(path.c_str());
        Bitmap* raw = bitmap->GetLastStatus() == Gdiplus::Ok ? bitmap.get() : nullptr;
        avatarCache.emplace(path, std::move(bitmap));
        return raw;
    }
};

XpRenderer::XpRenderer(XpTheme theme)
    : impl_(std::make_unique<Impl>()), theme_(std::move(theme)) {}

XpRenderer::~XpRenderer() = default;

void XpRenderer::SetTheme(XpTheme theme) {
    theme_ = std::move(theme);
    ReleaseResources();
}

void XpRenderer::SetUsers(std::vector<RenderUser> users) {
    impl_->users = std::move(users);
}

bool XpRenderer::LoadAssets(const std::wstring& packPath) {
    return impl_->assets.Load(packPath);
}

bool XpRenderer::UsingRealArtwork() const { return impl_->assets.Loaded(); }

void XpRenderer::ReleaseResources() {
    impl_->avatarCache.clear();
    impl_->headingFamily.reset();
    impl_->bodyFamily.reset();
    impl_->statusFamily.reset();
    impl_->ReleaseBuffer();
}

void XpRenderer::Paint(HDC target, const FrameLayout& frame,
                       const RenderState& state) {
    const int width = frame.screen.width;
    const int height = frame.screen.height;
    if (width <= 0 || height <= 0) {
        return;
    }

    if (!impl_->EnsureBuffer(target, width, height)) {
        // Last resort: a flat blue screen still lets the user see something is
        // there, which beats a black rectangle over the logon desktop.
        RECT full = {0, 0, width, height};
        HBRUSH brush = ::CreateSolidBrush(RGB(0x5A, 0x7E, 0xDC));
        ::FillRect(target, &full, brush);
        ::DeleteObject(brush);
        return;
    }

    HDC dc = impl_->memoryDc;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    if (!impl_->headingFamily) {
        impl_->headingFamily =
            impl_->Resolve(theme_.fonts.headingFamily, theme_.fonts.fallbackFamily);
        impl_->bodyFamily =
            impl_->Resolve(theme_.fonts.bodyFamily, theme_.fonts.fallbackFamily);
        impl_->statusFamily =
            impl_->Resolve(theme_.fonts.statusFamily, theme_.fonts.fallbackFamily);
    }

    const float scale = frame.scale;
    auto Scaled = [scale](float points) { return static_cast<REAL>(points * scale); };

    // ---- background --------------------------------------------------------
    FillGradient(graphics, frame.headerBand, theme_.colors.headerBand);
    FillGradient(graphics, frame.centerPanel, theme_.colors.centerPanel);
    FillGradient(graphics, frame.footerBand, theme_.colors.footerBand);

    // A soft light source in the upper left of the centre panel: the content of
    // the contentcontainer element, 219x207rp, top left. The bitmap is opaque
    // 24bpp and already contains rgb(90,126,220) everywhere it is not glowing,
    // which is exactly theme_.colors.centerPanel - so it drops onto the flat
    // fill without a seam. That is true of most of XP's artwork here: the
    // background is painted into the bitmap rather than keyed out of it, which
    // is why so many of them declare no colour key.
    if (Bitmap* glowArt = impl_->assets.Get(assets::assetid::kBackgroundGlow)) {
        const UiRect glowArea{
            frame.centerPanel.x,
            frame.centerPanel.y,
            static_cast<int>(glowArt->GetWidth() * scale),
            static_cast<int>(glowArt->GetHeight() * scale)};
        const GraphicsState saved = graphics.Save();
        graphics.SetClip(ToGdiRect(frame.centerPanel));
        graphics.DrawImage(glowArt, ToGdiRect(glowArea));
        graphics.Restore(saved);
    } else if (AlphaOf(theme_.colors.centerGlow) > 0) {
        const int glowRadius = static_cast<int>(
            theme_.colors.centerGlowRadius * frame.screen.width);
        const int glowX = static_cast<int>(
            theme_.colors.centerGlowX * frame.screen.width);
        const int glowY = static_cast<int>(
            theme_.colors.centerGlowY * frame.screen.height);
        const GpRect glowRect(glowX - glowRadius, glowY - glowRadius,
                              glowRadius * 2, glowRadius * 2);

        GraphicsPath glowPath;
        glowPath.AddEllipse(glowRect);
        PathGradientBrush glow(&glowPath);
        glow.SetCenterColor(ToColor(theme_.colors.centerGlow));
        Color edge[] = {Color(0, RedOf(theme_.colors.centerGlow),
                              GreenOf(theme_.colors.centerGlow),
                              BlueOf(theme_.colors.centerGlow))};
        int edgeCount = 1;
        glow.SetSurroundColors(edge, &edgeCount);

        // Clipped to the centre panel so it cannot bleed into the bands.
        const GraphicsState saved = graphics.Save();
        graphics.SetClip(ToGdiRect(frame.centerPanel));
        graphics.FillEllipse(&glow, glowRect);
        graphics.Restore(saved);
    }

    // The two dividers are the clearest case for using the real artwork: each
    // is an 800px sweep whose highlight sits about a third of the way across,
    // stretched to the screen width. A flat rule is visibly not the same thing.
    Bitmap* headerSweep = impl_->assets.Get(assets::assetid::kTopDivider);
    Bitmap* footerSweep = impl_->assets.Get(assets::assetid::kBottomDivider);

    if (headerSweep) {
        StretchImage(graphics, headerSweep, frame.headerDivider);
    } else if (!frame.headerDivider.IsEmpty()) {
        // No artwork: approximate the sweep with a bright core and a glow
        // falling away into the centre panel below it.
        DrawGlowLine(graphics, frame.headerDivider,
                     LerpArgb(theme_.colors.headerBand.Sample(1.0f), 0xFFFFFFFFu,
                              0.55f),
                     theme_.colors.centerGlow, /*glowUp=*/false);
    }

    if (footerSweep) {
        StretchImage(graphics, footerSweep, frame.footerAccent);
    } else if (!frame.footerAccent.IsEmpty()) {
        LinearGradientBrush accent(
            GpPoint(frame.footerAccent.x, frame.footerAccent.y),
            GpPoint(frame.footerAccent.x, frame.footerAccent.Bottom()),
            ToColor(theme_.colors.footerAccent),
            ToColor(theme_.colors.footerAccentGlow));
        graphics.FillRectangle(&accent, ToGdiRect(frame.footerAccent));
    }

    SolidBrush primaryBrush(ToColor(theme_.colors.primaryText));
    SolidBrush secondaryBrush(ToColor(theme_.colors.secondaryText));

    // ---- the logo lockup, in the centre column -----------------------------
    // Flag above and right of the wordmark, both above the instruction text.
    // The header band carries nothing at all.
    Bitmap* logoArt = impl_->assets.Get(assets::assetid::kLogo);
    if (logoArt && !frame.logo.IsEmpty()) {
        // The whole lockup - flag, "Microsoft", "Windows", the orange "xp" -
        // is one bitmap in logonui.exe, at exactly the size the markup gives
        // the element. Nothing to reconstruct.
        graphics.DrawImage(logoArt, ToGdiRect(frame.logo));
    } else if (!frame.logoFlag.IsEmpty()) {
        DrawWindowMark(graphics, frame.logoFlag);
    }
    if (!logoArt && !frame.logoWordmark.IsEmpty()) {
        Font wordmarkFont(impl_->headingFamily.get(),
                          Scaled(theme_.fonts.logoSize), Gdiplus::FontStyleBold,
                          Gdiplus::UnitPixel);
        Font microsoftFont(impl_->bodyFamily.get(),
                           Scaled(theme_.fonts.smallSize * 0.85f),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        UiRect microsoftArea = frame.logoWordmark;
        microsoftArea.height = static_cast<int>(14 * scale);
        graphics.DrawString(L"Microsoft", -1, &microsoftFont,
                            ToGdiRectF(microsoftArea), nullptr, &primaryBrush);

        UiRect wordArea = frame.logoWordmark;
        wordArea.y += microsoftArea.height;
        wordArea.height -= microsoftArea.height;
        graphics.DrawString(L"Windows", -1, &wordmarkFont, ToGdiRectF(wordArea),
                            nullptr, &primaryBrush);

        // The orange "xp", set right after the wordmark.
        GpRectF measured;
        graphics.MeasureString(L"Windows", -1, &wordmarkFont,
                               ToGdiRectF(wordArea), nullptr, &measured);
        SolidBrush orange(ToColor(theme_.colors.footerAccent));
        Font xpFont(impl_->headingFamily.get(),
                    Scaled(theme_.fonts.logoSize * 0.62f), Gdiplus::FontStyleBold,
                    Gdiplus::UnitPixel);
        UiRect xpArea = wordArea;
        xpArea.x += static_cast<int>(measured.Width) - static_cast<int>(4 * scale);
        graphics.DrawString(L"xp", -1, &xpFont, ToGdiRectF(xpArea), nullptr,
                            &orange);
    }

    // ---- centre column -----------------------------------------------------
    if (!frame.centerDivider.IsEmpty()) {
        // The rule fades out at both ends rather than stopping abruptly.
        LinearGradientBrush dividerBrush(
            GpPoint(frame.centerDivider.x, frame.centerDivider.y),
            GpPoint(frame.centerDivider.x, frame.centerDivider.Bottom()),
            Color(0, 255, 255, 255), Color(0, 255, 255, 255));
        Color colors[] = {Color(0, 255, 255, 255),
                          ToColor(theme_.colors.centerDivider),
                          Color(0, 255, 255, 255)};
        REAL positions[] = {0.0f, 0.5f, 1.0f};
        dividerBrush.SetInterpolationColors(colors, positions, 3);
        graphics.FillRectangle(&dividerBrush, ToGdiRect(frame.centerDivider));
    }

    if (!frame.instruction.IsEmpty()) {
        Font headingFont(impl_->headingFamily.get(),
                         Scaled(theme_.fonts.headingSize), Gdiplus::FontStyleRegular,
                         Gdiplus::UnitPixel);
        StringFormat format;
        // [UIFILE] contentalign=wrapright - right-aligned, wrapping.
        format.SetAlignment(Gdiplus::StringAlignmentFar);
        const std::wstring& text =
            state.statusText.empty() ? theme_.textClickUserName : state.statusText;
        graphics.DrawString(text.c_str(), -1, &headingFont,
                            ToGdiRectF(frame.instruction), &format, &primaryBrush);
    }

    // ---- tiles -------------------------------------------------------------
    Font nameFont(impl_->headingFamily.get(), Scaled(theme_.fonts.userNameSize),
                  Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Font statusFont(impl_->bodyFamily.get(), Scaled(theme_.fonts.statusSize),
                    Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    for (const TileLayout& tile : frame.tiles) {
        const RenderUser* user =
            (tile.userIndex >= 0 &&
             tile.userIndex < static_cast<int>(impl_->users.size()))
                ? &impl_->users[static_cast<size_t>(tile.userIndex)]
                : nullptr;

        const bool hovered = tile.userIndex == state.hoveredTile;
        // Everything in this tile is drawn through its opacity, which XpLayout
        // has already resolved from the hot/rest sheet rule.
        const float tileAlpha = std::clamp(tile.opacity, 0.0f, 1.0f);
        // [UIFILE] userpane[selected] background is rcbmp(112,...) with the
        // button sheet's borderthickness rect(8,8,0,8). Only the selected tile
        // gets it; hover is an alpha change in XP, not a plate.
        Bitmap* tilePlate =
            tile.selected ? impl_->assets.Get(assets::assetid::kSelectedTile) : nullptr;
        if (tilePlate) {
            DrawNineSlice(graphics, tilePlate, tile.bounds, {8, 8, 0, 8}, scale,
                          tileAlpha);
        } else if (hovered || tile.selected) {
            SolidBrush fill(Fade(tile.selected ? theme_.colors.tileSelectedFill
                                               : theme_.colors.tileHoverFill,
                                 tileAlpha));
            const int radius = (std::max)(2, static_cast<int>(6 * scale));
            const GpRect r = ToGdiRect(tile.bounds);
            GraphicsPath rounded;
            rounded.AddArc(r.X, r.Y, radius * 2, radius * 2, 180.0f, 90.0f);
            rounded.AddArc(r.GetRight() - radius * 2, r.Y, radius * 2, radius * 2,
                           270.0f, 90.0f);
            rounded.AddArc(r.GetRight() - radius * 2, r.GetBottom() - radius * 2,
                           radius * 2, radius * 2, 0.0f, 90.0f);
            rounded.AddArc(r.X, r.GetBottom() - radius * 2, radius * 2, radius * 2,
                           90.0f, 90.0f);
            rounded.CloseFigure();
            graphics.FillPath(&fill, &rounded);
        }

        // XP draws the account picture in two passes: a 58x58 plate, then the
        // 48x48 photo dropped into the well in the middle of it. Resource 113
        // is that plate - rounded transparent corners, a soft drop shadow baked
        // into the alpha down the right and bottom, and a flat opaque interior
        // at exactly x/y 5..52. Painting the photo first and the plate over it
        // would hide the photo completely; the plate is not a cut-out frame.
        // [UIFILE] pictureframe uses 119 for both [mousefocused] and
        // [selected], 113 otherwise, 9-sliced with rect(5,5,5,5).
        const bool frameHot = hovered || tile.selected;
        Bitmap* plateArt = impl_->assets.Get(frameHot ? assets::assetid::kPictureFrameHot
                                                      : assets::assetid::kPictureFrame);
        if (plateArt) {
            DrawNineSlice(graphics, plateArt, tile.avatar, {5, 5, 5, 5}, scale,
                          tileAlpha);
        } else {
            // No artwork: a white frame with a hand-drawn drop shadow. The
            // plate carries its own shadow, which is why this is the else.
            const UiRect shadow{tile.avatar.x + 2, tile.avatar.y + 2,
                                tile.avatar.width, tile.avatar.height};
            SolidBrush shadowBrush(Fade(theme_.colors.avatarShadow, tileAlpha));
            graphics.FillRectangle(&shadowBrush, ToGdiRect(shadow));
        }

        // The photo goes in the well when we have the plate, and fills the
        // whole 58x58 when we are drawing the frame ourselves.
        const UiRect pictureArea =
            plateArt && !tile.avatarPicture.IsEmpty() ? tile.avatarPicture : tile.avatar;

        Bitmap* avatar = user ? impl_->Avatar(user->avatarPath) : nullptr;
        if (!avatar) {
            avatar = impl_->assets.Get(assets::assetid::kDefaultPicture);
        }
        if (avatar) {
            DrawImageFaded(graphics, avatar, pictureArea, tileAlpha);
        } else if (!plateArt) {
            // Generic tile: a soft blue square with a person silhouette.
            LinearGradientBrush placeholder(
                GpPoint(tile.avatar.x, tile.avatar.y),
                GpPoint(tile.avatar.x, tile.avatar.Bottom()),
                Color(255, 0xC7, 0xD9, 0xF5), Color(255, 0x7E, 0xA2, 0xE0));
            graphics.FillRectangle(&placeholder, ToGdiRect(tile.avatar));

            SolidBrush silhouette(Color(200, 255, 255, 255));
            const int headSize = tile.avatar.width / 3;
            graphics.FillEllipse(&silhouette,
                                 tile.avatar.x + (tile.avatar.width - headSize) / 2,
                                 tile.avatar.y + tile.avatar.height / 6, headSize,
                                 headSize);
            graphics.FillEllipse(&silhouette, tile.avatar.x + tile.avatar.width / 6,
                                 tile.avatar.y + tile.avatar.height / 2,
                                 tile.avatar.width * 2 / 3, tile.avatar.height);
        }

        if (!plateArt) {
            Pen border(Fade(theme_.colors.avatarBorder, tileAlpha),
                       (std::max)(1.0f, theme_.metrics.avatarBorder * scale));
            graphics.DrawRectangle(&border, ToGdiRect(tile.avatar));
        }

        if (!user) {
            continue;
        }

        StringFormat format;
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        SolidBrush nameBrush(Fade(user->disabled ? theme_.colors.secondaryText
                                                  : theme_.colors.primaryText,
                                  tileAlpha));
        graphics.DrawString(user->displayName.c_str(), -1, &nameFont,
                            ToGdiRectF(tile.nameText), &format, &nameBrush);

        if (!user->statusLine.empty()) {
            SolidBrush statusBrush(Fade(theme_.colors.secondaryText, tileAlpha));
            graphics.DrawString(user->statusLine.c_str(), -1, &statusFont,
                                ToGdiRectF(tile.statusText), &format,
                                &statusBrush);
        }
    }

    // ---- password controls -------------------------------------------------
    if (frame.passwordViewActive && !frame.passwordBox.IsEmpty()) {
        UiRect box = frame.passwordBox;
        box.x += static_cast<int>(state.errorShakeOffset);

        // [UIFILE] edit[password] background is rcbmp(102,...) with
        // borderthickness rect(3,3,5,5) - a 171x26 plate with rounded ends,
        // stretched through the middle so the ends keep their shape.
        if (Bitmap* boxArt = impl_->assets.Get(assets::assetid::kPasswordBox)) {
            DrawNineSlice(graphics, boxArt, box, {3, 3, 5, 5}, scale);
        } else {
            SolidBrush fill(ToColor(theme_.colors.passwordBoxFill));
            graphics.FillRectangle(&fill, ToGdiRect(box));
            Pen boxBorder(ToColor(theme_.colors.passwordBoxBorder), 1.0f);
            graphics.DrawRectangle(&boxBorder, ToGdiRect(box));
        }

        // Bullets only. The password buffer itself never reaches the renderer.
        const std::wstring bullets(state.passwordLength, L'\x25CF');
        Font passwordFont(impl_->bodyFamily.get(), Scaled(theme_.fonts.bodySize),
                          Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        SolidBrush passwordBrush(ToColor(theme_.colors.passwordText));
        StringFormat format;
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingNone);

        UiRect textArea = box;
        textArea.x += static_cast<int>(4 * scale);
        textArea.width -= static_cast<int>(8 * scale);
        graphics.DrawString(bullets.c_str(), -1, &passwordFont, ToGdiRectF(textArea),
                            &format, &passwordBrush);

        if (state.passwordFocused && state.showCaret) {
            GpRectF measured;
            graphics.MeasureString(bullets.c_str(), -1, &passwordFont,
                                   ToGdiRectF(textArea), &format, &measured);
            const int caretX = textArea.x + static_cast<int>(measured.Width) + 1;
            Pen caret(ToColor(theme_.colors.passwordText), 1.0f);
            graphics.DrawLine(&caret, caretX, box.y + 3, caretX, box.Bottom() - 3);
        }

        // XP ships a second variant of each of these for [keyfocused]. We have
        // no per-button keyboard focus to key off, so hover drives it - that is
        // the same pixel set, shown on the signal the user actually gives.
        const bool goHot = state.hoveredTarget == HitTarget::GoButton;
        if (Bitmap* goArt = impl_->assets.Get(goHot ? assets::assetid::kGoButtonFocused
                                                    : assets::assetid::kGoButton)) {
            CenterImage(graphics, goArt, frame.goButton, scale);
        } else {
            DrawOrb(graphics, frame.goButton, theme_.colors.goButtonOuter,
                    theme_.colors.goButtonInner,
                    state.pressedTarget == HitTarget::GoButton, goHot);
            Pen arrow(Color(240, 255, 255, 255),
                      (std::max)(1.5f, frame.goButton.width / 10.0f));
            arrow.SetEndCap(Gdiplus::LineCapArrowAnchor);
            const int cy = frame.goButton.y + frame.goButton.height / 2;
            graphics.DrawLine(&arrow, frame.goButton.x + frame.goButton.width / 4, cy,
                              frame.goButton.Right() - frame.goButton.width / 4, cy);
        }

        const bool hintHot = state.hoveredTarget == HitTarget::HintButton;
        Bitmap* hintArt = impl_->assets.Get(hintHot ? assets::assetid::kHelpButtonFocused
                                                    : assets::assetid::kHelpButton);
        if (hintArt && !frame.hintButton.IsEmpty()) {
            CenterImage(graphics, hintArt, frame.hintButton, scale);
        } else if (!frame.hintButton.IsEmpty()) {
            DrawOrb(graphics, frame.hintButton, theme_.colors.hintButtonOuter,
                    theme_.colors.hintButtonInner,
                    state.pressedTarget == HitTarget::HintButton,
                    state.hoveredTarget == HitTarget::HintButton);
            Font glyphFont(impl_->headingFamily.get(),
                           static_cast<REAL>(frame.hintButton.height) * 0.62f,
                           Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            StringFormat centre;
            centre.SetAlignment(Gdiplus::StringAlignmentCenter);
            centre.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            SolidBrush white(Color(255, 255, 255, 255));
            graphics.DrawString(L"?", -1, &glyphFont, ToGdiRectF(frame.hintButton),
                                &centre, &white);
        }

        // XP has no back control here - Escape returns to the account list -
        // so there is no original artwork to use and this one stays drawn.
        if (!frame.backButton.IsEmpty()) {
            DrawOrb(graphics, frame.backButton, theme_.colors.goButtonOuter,
                    theme_.colors.goButtonInner,
                    state.pressedTarget == HitTarget::BackButton,
                    state.hoveredTarget == HitTarget::BackButton);
            Pen arrow(Color(240, 255, 255, 255),
                      (std::max)(1.5f, frame.backButton.width / 10.0f));
            arrow.SetEndCap(Gdiplus::LineCapArrowAnchor);
            const int cy = frame.backButton.y + frame.backButton.height / 2;
            graphics.DrawLine(&arrow,
                              frame.backButton.Right() - frame.backButton.width / 4,
                              cy, frame.backButton.x + frame.backButton.width / 4,
                              cy);
        }

        if (!state.errorText.empty() && !frame.errorText.IsEmpty()) {
            Font errorFont(impl_->bodyFamily.get(), Scaled(theme_.fonts.bodySize),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            SolidBrush errorBrush(ToColor(theme_.colors.errorText));
            graphics.DrawString(state.errorText.c_str(), -1, &errorFont,
                                ToGdiRectF(frame.errorText), nullptr, &errorBrush);
        }

        if (state.capsLockOn) {
            Font smallFont(impl_->bodyFamily.get(), Scaled(theme_.fonts.smallSize),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            UiRect capsArea = frame.errorText;
            capsArea.y = frame.passwordBox.Bottom() + static_cast<int>(6 * scale);
            capsArea.height = static_cast<int>(18 * scale);
            graphics.DrawString(L"Caps Lock is on", -1, &smallFont,
                                ToGdiRectF(capsArea), nullptr, &secondaryBrush);
        }
    }

    // ---- footer ------------------------------------------------------------
    // XP's turn-off button is a small rounded square hard against the left
    // edge, not one of the round orbs used inside the shutdown dialog.
    if (Bitmap* powerArt = impl_->assets.Get(assets::assetid::kPowerIcon)) {
        CenterImage(graphics, powerArt, frame.powerButton, scale);
    } else if (!frame.powerButton.IsEmpty()) {
        const GpRect r = ToGdiRect(frame.powerButton);
        const int radius = (std::max)(2, static_cast<int>(4 * scale));

        GraphicsPath rounded;
        rounded.AddArc(r.X, r.Y, radius * 2, radius * 2, 180.0f, 90.0f);
        rounded.AddArc(r.GetRight() - radius * 2, r.Y, radius * 2, radius * 2,
                       270.0f, 90.0f);
        rounded.AddArc(r.GetRight() - radius * 2, r.GetBottom() - radius * 2,
                       radius * 2, radius * 2, 0.0f, 90.0f);
        rounded.AddArc(r.X, r.GetBottom() - radius * 2, radius * 2, radius * 2,
                       90.0f, 90.0f);
        rounded.CloseFigure();

        const bool hovered = state.hoveredTarget == HitTarget::TurnOffButton;
        LinearGradientBrush body(
            GpPoint(r.X, r.Y), GpPoint(r.X, r.GetBottom()),
            ToColor(hovered ? LerpArgb(theme_.colors.powerButtonInner,
                                       0xFFFFFFFFu, 0.2f)
                            : theme_.colors.powerButtonInner),
            ToColor(theme_.colors.powerButtonOuter));
        graphics.FillPath(&body, &rounded);

        Pen rim(ToColor(LerpArgb(theme_.colors.powerButtonOuter, 0xFF000000u,
                                 0.35f)),
                (std::max)(1.0f, scale));
        graphics.DrawPath(&rim, &rounded);
        DrawPowerGlyph(graphics, frame.powerButton);
    }

    Font labelFont(impl_->bodyFamily.get(), Scaled(theme_.fonts.bodySize),
                   Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    {
        StringFormat format;
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(theme_.textTurnOff.c_str(), -1, &labelFont,
                            ToGdiRectF(frame.powerLabel), &format, &primaryBrush);
    }

    // The way through to a PIN or a fingerprint. Underlined, because on this
    // screen nothing else is, and somebody who needs it has to be able to find
    // it without being told.
    if (!frame.signinOptions.IsEmpty()) {
        const bool hot = state.hoveredTarget == HitTarget::SigninOptions;
        Font linkFont(impl_->bodyFamily.get(), Scaled(theme_.fonts.smallSize),
                      Gdiplus::FontStyleUnderline, Gdiplus::UnitPixel);
        SolidBrush linkBrush(
            ToColor(hot ? theme_.colors.primaryText : theme_.colors.secondaryText));
        StringFormat format;
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(theme_.textSigninOptions.c_str(), -1, &linkFont,
                            ToGdiRectF(frame.signinOptions), &format, &linkBrush);
    }

    if (!frame.passwordViewActive && !frame.hintFooter.IsEmpty()) {
        Font hintFont(impl_->bodyFamily.get(), Scaled(theme_.fonts.smallSize),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentFar);
        graphics.DrawString(theme_.textHintFooter.c_str(), -1, &hintFont,
                            ToGdiRectF(frame.hintFooter), &format, &secondaryBrush);
    }

    // ---- the busy screen ---------------------------------------------------
    //
    // Logging off, shutting down and standing by are this screen with the
    // account list gone and one large italic line where the logo would be.
    // [UIFILE] welcome and welcomeshadow are the same string drawn twice:
    //   welcome       fontstyle italic, fontweight bold, rcint(44)pt = 36pt,
    //                 padding rect(0,0,22rp,0), contentalign topright
    //   welcomeshadow the same, foreground rgb(49,81,181),
    //                 padding rect(2rp,3rp,20rp,0) - 2rp right, 3rp down
    // The shadow goes first, or it covers the text it is a shadow of.
    if (!frame.shellStatus.IsEmpty() && !state.shellStatusText.empty()) {
        Font statusFont(impl_->statusFamily.get(), Scaled(theme_.fonts.shellStatusSize),
                        Gdiplus::FontStyleBold | Gdiplus::FontStyleItalic,
                        Gdiplus::UnitPixel);
        StringFormat statusFormat;
        statusFormat.SetAlignment(Gdiplus::StringAlignmentFar); // contentalign topright
        statusFormat.SetTrimming(Gdiplus::StringTrimmingNone);

        SolidBrush shadowBrush(ToColor(theme_.colors.shellStatusShadow));
        graphics.DrawString(state.shellStatusText.c_str(), -1, &statusFont,
                            ToGdiRectF(frame.shellStatusShadow), &statusFormat,
                            &shadowBrush);

        SolidBrush statusBrush(ToColor(theme_.colors.shellStatusText));
        graphics.DrawString(state.shellStatusText.c_str(), -1, &statusFont,
                            ToGdiRectF(frame.shellStatus), &statusFormat,
                            &statusBrush);
    }

    // ---- turn off dialog ---------------------------------------------------
    //
    // This modal is msgina.dll's, not logonui.exe's: the background (#20142),
    // the Windows flag (#20143) and the ten orb frames (#20150) all come out
    // of that binary, and the geometry comes from its dialog template #20100.
    // Everything below falls back to the drawn version when the artwork is not
    // baked in, which is also what happens on a build with no XP files at all.
    if (!frame.dialog.IsEmpty()) {
        SolidBrush shade(ToColor(theme_.colors.dialogShade));
        graphics.FillRectangle(&shade, ToGdiRect(frame.dialogBackdrop));

        Bitmap* panel = impl_->assets.Get(assets::assetid::kShutdownPanel);
        if (panel) {
            StretchImage(graphics, panel, frame.dialog);
        } else {
            FillGradient(graphics, frame.dialog, theme_.colors.dialogBody);
            Pen dialogBorder(ToColor(theme_.colors.dialogBorder),
                             (std::max)(1.0f, 2.0f * scale));
            graphics.DrawRectangle(&dialogBorder, ToGdiRect(frame.dialog));
        }

        Bitmap* flag = impl_->assets.Get(assets::assetid::kShutdownFlag);
        if (flag && !frame.dialogFlag.IsEmpty()) {
            StretchImage(graphics, flag, frame.dialogFlag);
        }

        // The title sits in the navy band, vertically centred in it.
        Font titleFont(impl_->headingFamily.get(),
                       Scaled(theme_.fonts.headingSize * 0.8f),
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        StringFormat titleFormat;
        titleFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(theme_.textDialogTitle.c_str(), -1, &titleFont,
                            ToGdiRectF(frame.dialogTitle), &titleFormat,
                            &primaryBrush);

        struct DialogButton {
            const UiRect& bounds;
            const UiRect& label;
            const std::wstring& text;
            int    orbBase;   // frame index in the strip
            Argb   outer;     // only used without the artwork
            Argb   inner;
            HitTarget target;
            void (*glyph)(Graphics&, const UiRect&);
        };

        // Left to right on screen: Stand By, Turn Off, Restart. The strip
        // stores them in a different order, which is what orbBase is for.
        const DialogButton buttons[] = {
            {frame.dialogStandBy, frame.dialogStandByLabel, theme_.textStandBy,
             assets::shutdownorb::kStandBy,
             0xFFB88A16u, 0xFFF0C24Au, HitTarget::DialogStandBy, &DrawStandByGlyph},
            {frame.dialogTurnOff, frame.dialogTurnOffLabel, theme_.textTurnOffButton,
             assets::shutdownorb::kTurnOff,
             theme_.colors.powerButtonOuter, theme_.colors.powerButtonInner,
             HitTarget::DialogTurnOff, &DrawPowerGlyph},
            {frame.dialogRestart, frame.dialogRestartLabel, theme_.textRestart,
             assets::shutdownorb::kRestart,
             0xFF2A6F2Au, 0xFF6FC46Fu, HitTarget::DialogRestart, &DrawRestartGlyph},
        };

        Bitmap* orbs = impl_->assets.Get(assets::assetid::kShutdownOrbs);

        StringFormat centre;
        centre.SetAlignment(Gdiplus::StringAlignmentCenter);

        for (const DialogButton& button : buttons) {
            const bool pressed = state.pressedTarget == button.target;
            const bool hot = state.hoveredTarget == button.target;

            if (orbs) {
                const int frameIndex =
                    button.orbBase + (pressed ? assets::shutdownorb::kPressed
                                              : hot ? assets::shutdownorb::kHot
                                                    : assets::shutdownorb::kNormal);
                DrawStripFrame(graphics, orbs, assets::shutdownorb::kFrameSize,
                               frameIndex, button.bounds);
            } else {
                DrawOrb(graphics, button.bounds, button.outer, button.inner,
                        pressed, hot);
                button.glyph(graphics, button.bounds);
            }

            graphics.DrawString(button.text.c_str(), -1, &labelFont,
                                ToGdiRectF(button.label), &centre, &primaryBrush);
        }

        // Cancel is a plain XP push button, not an orb, and msgina has no
        // artwork for it - it is a real Win32 button in the original.
        SolidBrush cancelFill(Color(255, 0xEC, 0xE9, 0xD8));
        graphics.FillRectangle(&cancelFill, ToGdiRect(frame.dialogCancel));
        Pen cancelBorder(Color(255, 0x00, 0x33, 0x99), 1.0f);
        graphics.DrawRectangle(&cancelBorder, ToGdiRect(frame.dialogCancel));
        SolidBrush cancelText(Color(255, 0, 0, 0));
        StringFormat cancelFormat;
        cancelFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
        cancelFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(theme_.textCancel.c_str(), -1, &labelFont,
                            ToGdiRectF(frame.dialogCancel), &cancelFormat,
                            &cancelText);
    }

    // ---- present -----------------------------------------------------------
    ::BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
}

} // namespace xplogin::ui::win32
