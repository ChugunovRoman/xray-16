#include "StdAfx.h"

#include "level_world_draw.h"

#include "Include/xrRender/DebugRender.h"
#include "UIGameCustom.h"
#include "xrUICore/Windows/UIWindow.h"

#include <mutex>

namespace gw::world_draw
{
namespace
{
// A shape turned into lines: the vertices and the pairs of their indices (a line - two indices)
struct Lines
{
    u32 color = 0;
    xr_vector<Fvector> vertices;
    xr_vector<u16> pairs;

    u16 Add(const Fvector& v)
    {
        vertices.push_back(v);
        return u16(vertices.size() - 1);
    }
    void Pair(u16 a, u16 b)
    {
        pairs.push_back(a);
        pairs.push_back(b);
    }
};

// The shapes, set from Lua (the Game thread or the main one) and read by the render (the main thread): g_lock.
// g_version grows with every change: the render rebuilds its batches only then
std::mutex g_lock;
xr_unordered_map<shared_str, Lines> g_shapes;
u32 g_version = 0;
bool g_limit_logged = false; // the limit of shapes was told in the log (once until Clear)

// The batches of the render, by color (main thread only): the lines of all the shapes of one color together, at most
// kBatchVertices vertices in one (the indices are u16, the buffer of the debug renderer - 32767)
constexpr u32 kBatchVertices = 8192;
xr_vector<Lines> g_batches;
u32 g_built = u32(-1); // g_version the batches were built for

bool Check(pcstr function, pcstr id)
{
    if (!g_pGameLevel)
        return false;
    if (!id || !id[0] || xr_strlen(id) > kMaxIdLength)
    {
        Msg("! [world_draw] %s: bad id (empty or longer than %u characters)", function, kMaxIdLength);
        return false;
    }
    return true;
}

bool CheckValues(pcstr function, pcstr id, bool valid)
{
    if (!valid)
        Msg("! [world_draw] %s '%s': a bad value (not a number, or a radius / size out of 0..10000)", function, id);
    return valid;
}

bool ValidSize(float v) { return _valid(v) && v > 0.f && v <= 10000.f; }

// Adds or replaces the shape
bool Store(pcstr function, pcstr id, Lines&& lines, u32 color)
{
    lines.color = color | 0xff000000; // opaque: the shader of the debug lines has no blending
    std::lock_guard<std::mutex> guard(g_lock);
    const shared_str key = id;
    const auto it = g_shapes.find(key);
    if (it != g_shapes.end())
        it->second = std::move(lines);
    else
    {
        if (g_shapes.size() >= kMaxShapes)
        {
            if (!g_limit_logged)
                Msg("! [world_draw] %s '%s': the limit of %u shapes (remove the old ones)", function, id, kMaxShapes);
            g_limit_logged = true;
            return false;
        }
        g_shapes.emplace(key, std::move(lines));
    }
    ++g_version;
    return true;
}

// A circle: center + u * cos(a) * r + v * sin(a) * r
void AddCircle(Lines& lines, const Fvector& center, float radius, const Fvector& u, const Fvector& v, u32 segments)
{
    const u16 first = u16(lines.vertices.size());
    for (u32 i = 0; i < segments; ++i)
    {
        const float a = PI_MUL_2 * float(i) / float(segments);
        Fvector p;
        p.mad(center, u, _cos(a) * radius).mad(v, _sin(a) * radius);
        lines.Add(p);
    }
    for (u32 i = 0; i < segments; ++i)
        lines.Pair(u16(first + i), u16(first + (i + 1) % segments));
}

// The anchors (RS12): set by Lua, applied by the render before the UI - g_lock too. The windows are compared as
// pointers until their dialog is known to be drawn (alive) and the window is its child
struct AnchorData
{
    CUIWindow* parent = nullptr;
    Fvector pos;
    float dx = 0.f;
    float dy = 0.f;
};
xr_unordered_map<CUIWindow*, AnchorData> g_anchors;

// A point of the world on the screen, in the UI units (as game.world2ui): false - behind the camera or out of it
bool Project(const Fvector& pos, Fvector2& out)
{
    const Fmatrix& m = Device.mFullTransform;
    const float w = pos.x * m._14 + pos.y * m._24 + pos.z * m._34 + m._44;
    if (w <= EPS_S)
        return false;
    const float x = (pos.x * m._11 + pos.y * m._21 + pos.z * m._31 + m._41) / w;
    const float y = (pos.x * m._12 + pos.y * m._22 + pos.z * m._32 + m._42) / w;
    if (_abs(x) > 1.f || _abs(y) > 1.f)
        return false;
    out.set((1.f + x) * 0.5f * UI_BASE_WIDTH, (1.f - y) * 0.5f * UI_BASE_HEIGHT);
    return true;
}

void ApplyAnchors()
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_anchors.empty())
        return;
    CUIGameCustom* game_ui = CurrentGameUI();
    for (auto it = g_anchors.begin(); it != g_anchors.end();)
    {
        CUIWindow* wnd = it->first;
        const AnchorData& a = it->second;
        // the dialog is drawn (so alive) and the window is still its child: only then the pointers are used
        if (!game_ui || !game_ui->IsDialogToRender(a.parent) || !a.parent->IsChild(wnd))
        {
            it = g_anchors.erase(it);
            continue;
        }
        Fvector2 p;
        if (Project(a.pos, p))
        {
            wnd->SetWndPos(Fvector2().set(p.x + a.dx, p.y + a.dy));
            if (!wnd->IsShown())
                wnd->Show(true);
        }
        else if (wnd->IsShown())
            wnd->Show(false);
        ++it;
    }
}

void Rebuild()
{
    g_batches.clear();
    xr_unordered_map<u32, size_t> open; // color -> the batch of this color being filled
    for (const auto& [key, shape] : g_shapes)
    {
        const auto it = open.find(shape.color);
        size_t index;
        if (it == open.end() || g_batches[it->second].vertices.size() + shape.vertices.size() > kBatchVertices)
        {
            index = g_batches.size();
            g_batches.emplace_back();
            g_batches.back().color = shape.color;
            open[shape.color] = index;
        }
        else
            index = it->second;
        Lines& batch = g_batches[index];
        const u16 base = u16(batch.vertices.size());
        batch.vertices.insert(batch.vertices.end(), shape.vertices.begin(), shape.vertices.end());
        for (const u16 i : shape.pairs)
            batch.pairs.push_back(u16(base + i));
    }
}
} // namespace

bool Line(pcstr id, const Fvector& a, const Fvector& b, u32 color)
{
    if (!Check("draw_line", id) || !CheckValues("draw_line", id, _valid(a) && _valid(b)))
        return false;
    Lines lines;
    lines.Pair(lines.Add(a), lines.Add(b));
    return Store("draw_line", id, std::move(lines), color);
}

bool Circle(pcstr id, const Fvector& center, float radius, u32 color, u32 segments)
{
    if (!Check("draw_circle", id) || !CheckValues("draw_circle", id, _valid(center) && ValidSize(radius)))
        return false;
    if (segments == 0)
        segments = u32(clampr(iFloor(radius * 2.f), 16, 64)); // ~ every 3 m on a big ring, 16 on a small one
    clamp(segments, 3u, kMaxSegments);
    Lines lines;
    AddCircle(lines, center, radius, Fvector().set(1.f, 0.f, 0.f), Fvector().set(0.f, 0.f, 1.f), segments);
    return Store("draw_circle", id, std::move(lines), color);
}

bool Polyline(pcstr id, const xr_vector<Fvector>& points, bool closed, u32 color)
{
    if (!Check("draw_polyline", id))
        return false;
    if (points.size() < 2 || points.size() > kMaxPoints)
    {
        Msg("! [world_draw] draw_polyline '%s': %u points (2..%u)", id, u32(points.size()), kMaxPoints);
        return false;
    }
    Lines lines;
    for (const Fvector& p : points)
    {
        if (!CheckValues("draw_polyline", id, _valid(p)))
            return false;
        lines.Add(p);
    }
    const u16 n = u16(points.size());
    for (u16 i = 0; i + 1 < n; ++i)
        lines.Pair(i, u16(i + 1));
    if (closed && n > 2)
        lines.Pair(u16(n - 1), 0);
    return Store("draw_polyline", id, std::move(lines), color);
}

bool Box(pcstr id, const Fvector& center, const Fvector& half, const Fvector& hpb, u32 color)
{
    if (!Check("draw_box", id) ||
        !CheckValues("draw_box", id,
            _valid(center) && _valid(hpb) && ValidSize(half.x) && ValidSize(half.y) && ValidSize(half.z)))
    {
        return false;
    }
    Fmatrix xform;
    xform.setHPB(hpb.x, hpb.y, hpb.z);
    xform.translate_over(center);
    Lines lines;
    for (int i = 0; i < 8; ++i)
    {
        // the order of CDebugRenderer::draw_obb: (-,-,-) (-,+,-) (+,+,-) (+,-,-), then the same with +z
        const float sx = (i & 3) == 2 || (i & 3) == 3 ? 1.f : -1.f;
        const float sy = (i & 3) == 1 || (i & 3) == 2 ? 1.f : -1.f;
        const float sz = i >= 4 ? 1.f : -1.f;
        Fvector p;
        xform.transform_tiny(p, Fvector().set(sx * half.x, sy * half.y, sz * half.z));
        lines.Add(p);
    }
    static constexpr u16 edges[12 * 2] = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 1, 5, 2, 6, 3, 7, 0, 4};
    for (int i = 0; i < 12 * 2; i += 2)
        lines.Pair(edges[i], edges[i + 1]);
    return Store("draw_box", id, std::move(lines), color);
}

bool Sphere(pcstr id, const Fvector& center, float radius, u32 color)
{
    if (!Check("draw_sphere", id) || !CheckValues("draw_sphere", id, _valid(center) && ValidSize(radius)))
        return false;
    constexpr u32 segments = 16;
    const Fvector x = Fvector().set(1.f, 0.f, 0.f);
    const Fvector y = Fvector().set(0.f, 1.f, 0.f);
    const Fvector z = Fvector().set(0.f, 0.f, 1.f);
    Lines lines;
    AddCircle(lines, center, radius, x, z, segments);
    AddCircle(lines, center, radius, x, y, segments);
    AddCircle(lines, center, radius, z, y, segments);
    return Store("draw_sphere", id, std::move(lines), color);
}

bool Arrow(pcstr id, const Fvector& from, const Fvector& to, u32 color, float head)
{
    if (!Check("draw_arrow", id) || !CheckValues("draw_arrow", id, _valid(from) && _valid(to) && _valid(head)))
        return false;
    Lines lines;
    const u16 tip = lines.Add(to);
    lines.Pair(lines.Add(from), tip);
    Fvector dir;
    dir.sub(to, from);
    const float length = dir.magnitude();
    if (length > EPS_L)
    {
        dir.div(length);
        const float h = head > 0.f ? _min(head, length) : _min(length * 0.25f, 0.5f);
        // two directions across the arrow: horizontal and "up" (a vertical arrow - across x)
        Fvector side, up;
        up.set(0.f, 1.f, 0.f);
        if (_abs(dir.y) > 0.99f)
            up.set(1.f, 0.f, 0.f);
        side.crossproduct(dir, up).normalize();
        up.crossproduct(side, dir).normalize();
        Fvector back;
        back.mad(to, dir, -h);
        const float w = h * 0.5f;
        lines.Pair(lines.Add(Fvector().mad(back, side, w)), tip);
        lines.Pair(lines.Add(Fvector().mad(back, side, -w)), tip);
        lines.Pair(lines.Add(Fvector().mad(back, up, w)), tip);
        lines.Pair(lines.Add(Fvector().mad(back, up, -w)), tip);
    }
    return Store("draw_arrow", id, std::move(lines), color);
}

bool Remove(pcstr id)
{
    if (!id || !id[0])
        return false;
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_shapes.erase(shared_str(id)) == 0)
        return false;
    ++g_version;
    return true;
}

u32 RemovePrefix(pcstr prefix)
{
    if (!prefix || !prefix[0])
        return 0;
    const size_t length = xr_strlen(prefix);
    std::lock_guard<std::mutex> guard(g_lock);
    u32 removed = 0;
    for (auto it = g_shapes.begin(); it != g_shapes.end();)
    {
        if (0 == strncmp(it->first.c_str(), prefix, length))
        {
            it = g_shapes.erase(it);
            ++removed;
        }
        else
            ++it;
    }
    if (removed)
        ++g_version;
    return removed;
}

void RemoveAll()
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_shapes.empty())
        ++g_version;
    g_shapes.clear();
}

u32 Count()
{
    std::lock_guard<std::mutex> guard(g_lock);
    return u32(g_shapes.size());
}

bool Anchor(CUIWindow* wnd, const Fvector& pos, float dx, float dy)
{
    if (!g_pGameLevel || !wnd)
        return false;
    CUIWindow* parent = wnd->GetParent();
    if (!parent)
    {
        Msg("! [world_draw] ui_anchor: the window has no parent (it must be a child of a dialog of the HUD)");
        return false;
    }
    if (!_valid(pos) || !_valid(dx) || !_valid(dy))
    {
        Msg("! [world_draw] ui_anchor: a bad value (not a number)");
        return false;
    }
    std::lock_guard<std::mutex> guard(g_lock);
    const auto it = g_anchors.find(wnd);
    if (it == g_anchors.end() && g_anchors.size() >= kMaxAnchors)
    {
        if (!g_limit_logged)
            Msg("! [world_draw] ui_anchor: the limit of %u anchors", kMaxAnchors);
        g_limit_logged = true;
        return false;
    }
    AnchorData& a = g_anchors[wnd];
    a.parent = parent;
    a.pos = pos;
    a.dx = dx;
    a.dy = dy;
    return true;
}

bool AnchorRemove(CUIWindow* wnd)
{
    std::lock_guard<std::mutex> guard(g_lock);
    return g_anchors.erase(wnd) != 0;
}

void AnchorClear()
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_anchors.clear();
}

void Render()
{
    // the anchored windows first: they do not need the debug renderer; the second viewport (a 3D scope) has the
    // camera of the scope - the windows keep the positions of the main view
    if (!Device.m_SecondViewport.IsSVPFrame())
        ApplyAnchors();
    if (!GEnv.DRender)
        return;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_built != g_version)
        {
            Rebuild();
            g_built = g_version;
        }
    }
    if (g_batches.empty())
        return;
    IDebugRender& render = *GEnv.DRender;
    // The lines other code queued this frame (CDebugRenderer: the HUD tuner) - drawn now, with their own color
    render.Render();
    // dxDebugRender::Render (DX11 / GL) draws a batch by the "editor\wire" shader: the color of a vertex times tfactor,
    // and tfactor is the color of the first vertex of the batch. So one batch - one color: the first vertex (no line
    // uses it) has the color, the vertices of the lines are white
    const Fvector origin = Fvector().set(0.f, 0.f, 0.f);
    const u32 one = 1;
    const u32 none = 0;
    for (const Lines& batch : g_batches)
    {
        const u32 vertex_count = u32(batch.vertices.size());
        const u32 pair_count = u32(batch.pairs.size() / 2);
        render.add_lines(&origin, one, nullptr, none, batch.color);
        render.add_lines(batch.vertices.data(), vertex_count, batch.pairs.data(), pair_count, color_rgba(255, 255, 255, 255));
        render.Render();
    }
}

void Clear()
{
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_shapes.clear();
        g_anchors.clear();
        ++g_version;
        g_limit_logged = false;
    }
    g_batches.clear();
    g_built = u32(-1);
}
} // namespace gw::world_draw
