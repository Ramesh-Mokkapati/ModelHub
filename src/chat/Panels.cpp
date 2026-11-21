#include "Panels.h"

#include "Util.h"

#include <filesystem>

#define SECURITY_WIN32
#include <security.h>
#pragma comment(lib, "secur32.lib")

namespace fs = std::filesystem;

namespace chat
{

namespace
{

constexpr COLORREF kSideBg = RGB(246, 246, 247);
constexpr COLORREF kInfoBg = RGB(250, 250, 251);
constexpr COLORREF kHover = RGB(234, 234, 237);
constexpr COLORREF kSelected = RGB(226, 226, 230);
constexpr COLORREF kText = RGB(32, 33, 36);
constexpr COLORREF kMuted = RGB(110, 110, 118);
constexpr COLORREF kLine = RGB(229, 229, 232);
constexpr COLORREF kAccent = RGB(37, 99, 235);

// Windows' icon font glyphs.
constexpr wchar_t kAdd = 0xE710, kSearch = 0xE721, kCode = 0xE943, kCloud = 0xE753, kSettings = 0xE713,
                  kChevronDown = 0xE70D, kChevronRight = 0xE76C, kClosePane = 0xE89F, kOpenPane = 0xE8A0,
                  kLaptop = 0xE7F8, kFolder = 0xE8B7, kDocument = 0xE8A5, kClose = 0xE8BB, kPage = 0xE7C3,
                  kTools = 0xE90F, kTerminal = 0xE756, kFolderOpen = 0xE838, kMore = 0xE712;

const wchar_t* IconFace()
{
    static const wchar_t* face = []
    {
        bool fluent = false;
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(
            dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int
            {
                *reinterpret_cast<bool*>(p) = true;
                return 0; },
            reinterpret_cast<LPARAM>(&fluent), 0);
        ReleaseDC(nullptr, dc);
        return fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return face;
}

std::wstring UserDisplayName()
{
    wchar_t buf[256];
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, buf, &n) && n > 0)
    {
        return std::wstring(buf, n);
    }

    n = 256;
    return GetUserNameW(buf, &n) && n > 1 ? std::wstring(buf, n - 1) : L"You";
}

}  // namespace

// ---------------------------------------------------------------- the base

BEGIN_MESSAGE_MAP(CPanelBase, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_MOUSEMOVE()
    ON_MESSAGE(WM_MOUSELEAVE, &CPanelBase::OnMouseLeave)
    ON_WM_LBUTTONUP()
    ON_WM_RBUTTONUP()
    ON_WM_MOUSEWHEEL()
    ON_WM_SETCURSOR()
END_MESSAGE_MAP()

BOOL CPanelBase::CreatePanel(CWnd* parent, UINT id)
{
    CString cls = AfxRegisterWndClass(CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr);
    return CreateEx(0, cls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, CRect(0, 0, 10, 10), parent, id);
}

int CPanelBase::Px(int v) const
{
    return MulDiv(v, m_hWnd ? static_cast<int>(GetDpiForWindow(m_hWnd)) : 96, 96);
}

void CPanelBase::EnsureFonts()
{
    UINT dpi = GetDpiForWindow(m_hWnd);
    if (dpi == fontDpi_)
    {
        return;
    }

    fontDpi_ = dpi;
    auto make = [dpi](CFont& f, int pt, int weight, const wchar_t* face)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi), 72);
        lf.lfWeight = weight;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, face);
        f.DeleteObject();
        f.CreateFontIndirectW(&lf);
    };

    make(font_, 10, FW_NORMAL, L"Segoe UI");
    make(small_, 9, FW_NORMAL, L"Segoe UI");
    make(bold_, 10, FW_SEMIBOLD, L"Segoe UI");
    make(title_, 12, FW_SEMIBOLD, L"Segoe UI");
    make(icon_, 11, FW_NORMAL, IconFace());
}

void CPanelBase::AddHit(const CRect& rc, int id, size_t data)
{
    hits_.push_back({ rc, id, data });
}

bool CPanelBase::Hovered(int id, size_t data) const
{
    return hoverId_ == id && hoverData_ == data;
}

void CPanelBase::Icon(CDC& dc, wchar_t glyph, const CRect& rc, COLORREF color, UINT format)
{
    CFont* old = dc.SelectObject(&icon_);
    dc.SetTextColor(color);
    wchar_t s[2] = { glyph, 0 };
    CRect r = rc;
    dc.DrawText(s, 1, r, format | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    dc.SelectObject(old);
}

void CPanelBase::Text(CDC& dc, const std::wstring& s, CRect rc, COLORREF color, CFont& font, UINT format)
{
    CFont* old = dc.SelectObject(&font);
    dc.SetTextColor(color);
    dc.DrawText(s.c_str(), static_cast<int>(s.size()), rc, format | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    dc.SelectObject(old);
}

void CPanelBase::Fill(CDC& dc, const CRect& rc, COLORREF color, int radius)
{
    if (radius <= 0)
    {
        dc.FillSolidRect(rc, color);
        return;
    }

    CBrush brush(color);
    CPen pen(PS_SOLID, 1, color);
    CBrush* ob = dc.SelectObject(&brush);
    CPen* op = dc.SelectObject(&pen);
    dc.RoundRect(rc, CPoint(radius, radius));
    dc.SelectObject(ob);
    dc.SelectObject(op);
}

void CPanelBase::OnPaint()
{
    CPaintDC paint(this);
    EnsureFonts();
    CRect rc;
    GetClientRect(&rc);
    CDC mem;
    mem.CreateCompatibleDC(&paint);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(&paint, rc.Width(), rc.Height());
    CBitmap* old = mem.SelectObject(&bmp);
    mem.FillSolidRect(rc, Background());
    mem.SetBkMode(TRANSPARENT);
    hits_.clear();
    Draw(mem, rc);
    paint.BitBlt(0, 0, rc.Width(), rc.Height(), &mem, 0, 0, SRCCOPY);
    mem.SelectObject(old);
}

BOOL CPanelBase::OnEraseBkgnd(CDC*)
{
    return TRUE;
}

const CPanelBase::Hit* CPanelBase::HitAt(CPoint pt) const
{
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
    {
        if (it->rc.PtInRect(pt))
        {
            return &*it;
        }
    }

    return nullptr;
}

void CPanelBase::OnMouseMove(UINT, CPoint pt)
{
    if (!tracking_)
    {
        TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, m_hWnd, 0 };
        tracking_ = TrackMouseEvent(&t) != FALSE;
    }

    const Hit* h = HitAt(pt);
    int id = h ? h->id : -1;
    size_t data = h ? h->data : 0;
    if (id != hoverId_ || data != hoverData_)
    {
        hoverId_ = id;
        hoverData_ = data;
        Invalidate(FALSE);
    }
}

LRESULT CPanelBase::OnMouseLeave(WPARAM, LPARAM)
{
    tracking_ = false;
    if (hoverId_ != -1)
    {
        hoverId_ = -1;
        Invalidate(FALSE);
    }

    return 0;
}

void CPanelBase::OnLButtonUp(UINT, CPoint pt)
{
    if (const Hit* h = HitAt(pt))
    {
        CPoint screen = pt;
        ClientToScreen(&screen);
        Hit copy = *h;  // the handler may redraw (and rebuild the list)
        OnHit(copy.id, copy.data, false, screen);
    }
}

void CPanelBase::OnRButtonUp(UINT, CPoint pt)
{
    if (const Hit* h = HitAt(pt))
    {
        CPoint screen = pt;
        ClientToScreen(&screen);
        Hit copy = *h;
        OnHit(copy.id, copy.data, true, screen);
    }
}

BOOL CPanelBase::OnMouseWheel(UINT, short delta, CPoint)
{
    OnScroll(delta);
    return TRUE;
}

BOOL CPanelBase::OnSetCursor(CWnd* wnd, UINT hit, UINT msg)
{
    CPoint pt;
    GetCursorPos(&pt);
    ScreenToClient(&pt);
    if (hit == HTCLIENT && HitAt(pt))
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }

    return CWnd::OnSetCursor(wnd, hit, msg);
}

// ---------------------------------------------------------------- left: the sidebar

BEGIN_MESSAGE_MAP(CSidebarPanel, CPanelBase)
    ON_WM_CREATE()
    ON_WM_CTLCOLOR()
    ON_EN_CHANGE(1, &CSidebarPanel::OnSearchChange)
END_MESSAGE_MAP()

int CSidebarPanel::OnCreate(LPCREATESTRUCT cs)
{
    if (CPanelBase::OnCreate(cs) == -1)
    {
        return -1;
    }

    EnsureFonts();
    search_.Create(WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, CRect(0, 0, 10, 10), this, 1);
    search_.SetFont(&font_);
    search_.SetCueBanner(L"Search");
    searchBrush_.CreateSolidBrush(kSideBg);
    user_ = UserDisplayName();
    return 0;
}

HBRUSH CSidebarPanel::OnCtlColor(CDC* dc, CWnd*, UINT)
{
    dc->SetBkColor(kSideBg);
    dc->SetTextColor(kText);
    return searchBrush_;
}

void CSidebarPanel::OnSearchChange()
{
    if (onSearch)
    {
        onSearch();
    }
}

std::wstring CSidebarPanel::Filter() const
{
    CString s;
    search_.GetWindowText(s);
    return std::wstring(s.GetString());
}

void CSidebarPanel::SetGroups(std::vector<Group> groups)
{
    groups_ = std::move(groups);
    Invalidate(FALSE);
}

void CSidebarPanel::SetMcpLabel(const std::wstring& label)
{
    mcpLabel_ = label;
    Invalidate(FALSE);
}

COLORREF CSidebarPanel::Background() const
{
    return kSideBg;
}

void CSidebarPanel::OnScroll(int delta)
{
    int view = listBottom_ - listTop_;
    int maxScroll = std::max(0, contentHeight_ - view);
    scroll_ = std::clamp(scroll_ - delta * Px(40) / WHEEL_DELTA, 0, maxScroll);
    Invalidate(FALSE);
}

void CSidebarPanel::Draw(CDC& dc, const CRect& rc)
{
    const int pad = Px(10), rowH = Px(34), radius = Px(8);
    int y = Px(10);

    // Header: the application and the button that hides the panel.
    Text(dc, L"ModelHub Chat", CRect(pad + Px(6), y, rc.right - Px(48), y + Px(32)), kText, title_);
    CRect collapse(rc.right - pad - Px(32), y, rc.right - pad, y + Px(32));
    if (Hovered(NavCollapse))
    {
        Fill(dc, collapse, kHover, radius);
    }

    Icon(dc, kClosePane, collapse, kMuted);
    AddHit(collapse, NavCollapse);
    y += Px(44);

    // Navigation rows; Search holds the edit box.
    auto navRow = [&](int id, wchar_t glyph, const std::wstring& label)
    {
        CRect row(pad, y, rc.right - pad, y + rowH);
        if (id && Hovered(id))
        {
            Fill(dc, row, kHover, radius);
        }

        Icon(dc, glyph, CRect(row.left + Px(6), row.top, row.left + Px(30), row.bottom), kText);
        if (id)
        {
            Text(dc, label, CRect(row.left + Px(38), row.top, row.right - Px(6), row.bottom), kText, font_);
            AddHit(row, id);
        }
        else
        {
            search_.SetWindowPos(nullptr, row.left + Px(38), row.top + (rowH - Px(20)) / 2, row.Width() - Px(44), Px(20),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        y += rowH + Px(2);
    };

    navRow(NavNewChat, kAdd, L"New chat");
    navRow(0, kSearch, L"");
    navRow(NavMcp, kCode, mcpLabel_);
    navRow(NavProviders, kCloud, L"Model Providers");
    navRow(NavSettings, kSettings, L"Settings");

    y += Px(12);
    Text(dc, L"Recent conversations", CRect(pad + Px(8), y, rc.right - pad, y + Px(22)), kMuted, small_);
    y += Px(26);

    // Conversations, grouped by project (workspace folder); the list scrolls between header and footer.
    const int footerH = Px(56);
    listTop_ = y;
    listBottom_ = rc.bottom - footerH;
    CRgn clip;
    clip.CreateRectRgn(0, listTop_, rc.right, listBottom_);
    dc.SelectClipRgn(&clip);
    int cy = listTop_ - scroll_;
    for (size_t g = 0; g < groups_.size(); ++g)
    {
        const Group& grp = groups_[g];
        bool open = !collapsed_.count(grp.key);
        CRect head(pad, cy, rc.right - pad, cy + Px(30));
        if (head.bottom > listTop_ && head.top < listBottom_)
        {
            if (Hovered(NavGroup, g))
            {
                Fill(dc, head, kHover, radius);
            }

            Text(dc, grp.label, CRect(head.left + Px(8), head.top, head.right - Px(28), head.bottom), kMuted, bold_);
            Icon(dc, open ? kChevronDown : kChevronRight, CRect(head.right - Px(26), head.top, head.right - Px(4), head.bottom),
                kMuted);
            CRect visible = head;
            visible.IntersectRect(head, CRect(0, listTop_, rc.right, listBottom_));
            AddHit(visible, NavGroup, g);
        }

        cy += Px(32);
        if (!open)
        {
            continue;
        }

        for (const auto& c : grp.items)
        {
            CRect row(pad, cy, rc.right - pad, cy + Px(32));
            if (row.bottom > listTop_ && row.top < listBottom_)
            {
                if (c.selected)
                {
                    Fill(dc, row, kSelected, radius);
                }
                else if (Hovered(NavConversation, c.index))
                {
                    Fill(dc, row, kHover, radius);
                }

                Text(dc, c.title, CRect(row.left + Px(16), row.top, row.right - Px(8), row.bottom), kText, font_);
                CRect visible = row;
                visible.IntersectRect(row, CRect(0, listTop_, rc.right, listBottom_));
                AddHit(visible, NavConversation, c.index);
            }

            cy += Px(34);
        }

        cy += Px(6);
    }

    contentHeight_ = cy + scroll_ - listTop_;
    dc.SelectClipRgn(nullptr);

    // Footer: who is signed in to Windows, and Settings.
    CRect footer(0, rc.bottom - footerH, rc.right, rc.bottom);
    dc.FillSolidRect(CRect(pad, footer.top, rc.right - pad, footer.top + 1), kLine);
    CRect avatar(pad + Px(6), footer.top + Px(14), pad + Px(34), footer.top + Px(42));
    CBrush ab(RGB(217, 119, 87));
    CPen ap(PS_SOLID, 1, RGB(217, 119, 87));
    CBrush* ob = dc.SelectObject(&ab);
    CPen* op = dc.SelectObject(&ap);
    dc.Ellipse(avatar);
    dc.SelectObject(ob);
    dc.SelectObject(op);

    std::wstring initial = user_.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(towupper(user_[0])));
    Text(dc, initial, avatar, RGB(255, 255, 255), bold_, DT_CENTER);
    Text(dc, user_, CRect(avatar.right + Px(10), footer.top + Px(8), rc.right - Px(50), footer.bottom - Px(8)), kText, font_);

    CRect gear(rc.right - pad - Px(32), footer.top + Px(12), rc.right - pad, footer.top + Px(44));
    if (Hovered(NavSettings, 1))
    {
        Fill(dc, gear, kHover, radius);
    }

    Icon(dc, kSettings, gear, kMuted);
    AddHit(gear, NavSettings, 1);
}

void CSidebarPanel::OnHit(int id, size_t data, bool right, CPoint screen)
{
    if (id == NavGroup && !right)
    {
        if (data < groups_.size())
        {
            const std::wstring& key = groups_[data].key;
            if (!collapsed_.erase(key))
                collapsed_.insert(key);
            Invalidate(FALSE);
        }

        return;
    }

    if (onNav)
    {
        onNav(id, data, right, screen);
    }
}

// ---------------------------------------------------------------- right: environment, context, changes

COLORREF CInfoPanel::Background() const
{
    return kInfoBg;
}

void CInfoPanel::SetData(Data d)
{
    data_ = std::move(d);
    Invalidate(FALSE);
}

void CInfoPanel::Draw(CDC& dc, const CRect& rc)
{
    const int pad = Px(14), rowH = Px(30), radius = Px(8);
    dc.FillSolidRect(CRect(0, 0, 1, rc.bottom), kLine);
    int y = Px(12);

    auto section = [&](const std::wstring& title)
    {
        Text(dc, title, CRect(pad, y, rc.right - pad, y + Px(24)), kText, bold_);
        y += Px(28);
    };

    auto row = [&](wchar_t glyph, const std::wstring& text, const std::wstring& note, int id, size_t data = 0, int reserve = 0)
    {
        CRect r(pad - Px(4), y, rc.right - pad + Px(4), y + rowH);
        if (id && Hovered(id, data))
        {
            Fill(dc, r, kHover, radius);
        }

        Icon(dc, glyph, CRect(r.left + Px(4), r.top, r.left + Px(26), r.bottom), kMuted);
        CDC* pdc = &dc;
        CFont* old = pdc->SelectObject(&font_);
        CSize size = pdc->GetTextExtent(text.c_str(), static_cast<int>(text.size()));
        pdc->SelectObject(old);
        int textRight = std::min<int>(r.left + Px(34) + size.cx, r.right - Px(8));
        Text(dc, text, CRect(r.left + Px(34), r.top, textRight, r.bottom), kText, font_);
        if (!note.empty())
        {
            Text(dc, L"·  " + note, CRect(textRight + Px(8), r.top, r.right - Px(6) - reserve, r.bottom), kMuted, small_);
        }

        if (id)
        {
            AddHit(r, id, data);
        }

        y += rowH;
    };

    // Close button.
    CRect close(rc.right - Px(40), Px(10), rc.right - Px(10), Px(40));
    if (Hovered(ActClose))
    {
        Fill(dc, close, kHover, radius);
    }

    Icon(dc, kClose, close, kMuted);
    AddHit(close, ActClose);

    section(L"Environment");
    row(kLaptop, L"This computer", data_.computer, 0);
    row(kFolder, data_.folderName.empty() ? L"(no folder)" : data_.folderName, data_.folderPath, ActOpenFolder, 0, Px(60));
    {
        // "Change" at the end of the folder row.
        CRect change(rc.right - pad - Px(56), y - rowH, rc.right - pad, y);
        Text(dc, L"Change", change, Hovered(ActChangeFolder) ? RGB(29, 78, 216) : kAccent, small_, DT_RIGHT);
        AddHit(change, ActChangeFolder);
    }

    CRect branch(pad - Px(4), y, rc.right - pad, y + rowH);
    Text(dc, L"⎇", CRect(branch.left + Px(4), branch.top, branch.left + Px(26), branch.bottom), kMuted, font_, DT_CENTER);
    Text(dc, data_.branch.empty() ? L"Not a Git repository" : data_.branch, CRect(branch.left + Px(34), branch.top, branch.right, branch.bottom),
        data_.branch.empty() ? kMuted : kText, font_);
    y += rowH;
    row(kPage, data_.model, data_.service, 0);

    y += Px(10);
    dc.FillSolidRect(CRect(pad, y, rc.right - pad, y + 1), kLine);
    y += Px(10);

    // Context: how full the model's window is.
    CRect ctx(pad - Px(4), y, rc.right - pad + Px(4), y + rowH);
    if (Hovered(ActToggleContext))
    {
        Fill(dc, ctx, kHover, radius);
    }

    Text(dc, L"Context", CRect(ctx.left + Px(4), ctx.top, ctx.left + Px(70), ctx.bottom), kText, bold_);
    Icon(dc, contextOpen_ ? kChevronDown : kChevronRight, CRect(ctx.left + Px(70), ctx.top, ctx.left + Px(90), ctx.bottom), kMuted);
    int pct = std::clamp(data_.contextPercent, 0, 100);
    CRect bar(ctx.right - Px(40), ctx.top + rowH / 2 - Px(3), ctx.right - Px(4), ctx.top + rowH / 2 + Px(3));
    Fill(dc, bar, kLine, Px(6));
    CRect filled = bar;
    filled.right = bar.left + std::max(Px(6), bar.Width() * pct / 100);
    Fill(dc, filled, pct > 85 ? RGB(220, 38, 38) : pct > 60 ? RGB(217, 119, 6)
                                                            : kAccent,
        Px(6));

    Text(dc, std::to_wstring(pct) + L"%", CRect(ctx.left + Px(90), ctx.top, bar.left - Px(8), ctx.bottom), kMuted, small_, DT_RIGHT);
    AddHit(ctx, ActToggleContext);
    y += rowH;
    Text(dc, data_.contextSummary, CRect(pad, y, rc.right - pad, y + Px(20)), kMuted, small_);
    y += Px(22);
    if (contextOpen_)
    {
        for (const auto& [label, value] : data_.contextDetails)
        {
            Text(dc, label, CRect(pad + Px(8), y, rc.right - pad - Px(70), y + Px(20)), kMuted, small_);
            Text(dc, value, CRect(rc.right - pad - Px(70), y, rc.right - pad, y + Px(20)), kText, small_, DT_RIGHT);
            y += Px(20);
        }
    }

    y += Px(10);
    dc.FillSolidRect(CRect(pad, y, rc.right - pad, y + 1), kLine);
    y += Px(10);

    // Changes: files the tools wrote or edited in this conversation.
    Text(dc, L"Changes", CRect(pad, y, rc.right - pad, y + Px(24)), kText, bold_);
    Text(dc, data_.changes.empty() ? L"none" : std::to_wstring(data_.changes.size()) + (data_.changes.size() == 1 ? L" file" : L" files"),
        CRect(pad, y, rc.right - pad, y + Px(24)), kMuted, small_, DT_RIGHT);
    y += Px(28);
    for (size_t i = 0; i < data_.changes.size() && y < rc.bottom - rowH; ++i)
    {
        row(kDocument, data_.changes[i].first, L"", ActOpenChange, i);
    }
}

void CInfoPanel::OnHit(int id, size_t data, bool right, CPoint)
{
    if (right)
    {
        return;
    }

    if (id == ActToggleContext)
    {
        contextOpen_ = !contextOpen_;
        Invalidate(FALSE);
        return;
    }

    if (onAction)
    {
        onAction(id, data);
    }
}

// ---------------------------------------------------------------- the title strip

COLORREF CTitleStrip::Background() const
{
    return RGB(255, 255, 255);
}

void CTitleStrip::SetState(const std::wstring& title, bool sidebarVisible, bool infoVisible)
{
    title_ = title;
    sidebar_ = sidebarVisible;
    info_ = infoVisible;
    Invalidate(FALSE);
}

void CTitleStrip::Draw(CDC& dc, const CRect& rc)
{
    const int radius = Px(8), size = Px(32), gap = Px(2), top = (rc.Height() - size) / 2;
    int x = Px(8);
    auto button = [&](int id, wchar_t glyph, int left)
    {
        CRect r(left, top, left + size, top + size);
        if (Hovered(id))
        {
            Fill(dc, r, kHover, radius);
        }

        Icon(dc, glyph, r, kMuted);
        AddHit(r, id);
        buttons_[id] = r;
    };

    buttons_.clear();
    if (!sidebar_)
    {
        button(BtnShowSidebar, kOpenPane, x);
        x += size + Px(8);
    }

    // The toolbar, right to left: More, Folder, Terminal, Tools, and the right panel's toggle.
    int right = rc.right - Px(8);
    const std::pair<int, wchar_t> tools[] = { { BtnMore, kMore }, { BtnFolder, kFolderOpen }, { BtnTerminal, kTerminal },
        { BtnTools, kTools }, { BtnToggleInfo, info_ ? kClosePane : kOpenPane } };
    for (const auto& [id, glyph] : tools)
    {
        right -= size;
        button(id, glyph, right);
        right -= gap;
    }

    Text(dc, title_, CRect(x + Px(4), 0, right - Px(12), rc.bottom), kText, bold_);
    dc.FillSolidRect(CRect(0, rc.bottom - 1, rc.right, rc.bottom), kLine);

    // Tooltips for the icon-only buttons.
    if (!tips_.m_hWnd)
    {
        tips_.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX);
        const std::pair<int, const wchar_t*> names[] = { { BtnShowSidebar, L"Show the sidebar" },
            { BtnToggleInfo, L"Show or hide Environment and Context" }, { BtnTools, L"Tools" },
            { BtnTerminal, L"Open a terminal in the workspace folder" }, { BtnFolder, L"Open the workspace folder" },
            { BtnMore, L"More" } };

        for (const auto& [id, name] : names)
        {
            tips_.AddTool(this, name, CRect(0, 0, 0, 0), static_cast<UINT_PTR>(id));
        }
    }

    for (int id = BtnShowSidebar; id <= BtnMore; ++id)
    {
        auto it = buttons_.find(id);
        tips_.SetToolRect(this, static_cast<UINT_PTR>(id), it == buttons_.end() ? CRect(0, 0, 0, 0) : it->second);
    }
}

BOOL CTitleStrip::PreTranslateMessage(MSG* msg)
{
    if (tips_.m_hWnd)
    {
        tips_.RelayEvent(msg);
    }

    return CPanelBase::PreTranslateMessage(msg);
}

void CTitleStrip::OnHit(int id, size_t, bool right, CPoint)
{
    if (right || !onButton)
    {
        return;
    }

    CPoint screen(0, 0);
    auto it = buttons_.find(id);
    if (it != buttons_.end())
    {
        screen = CPoint(it->second.right, it->second.bottom + Px(2));
        ClientToScreen(&screen);
    }

    onButton(id, screen);
}

// ---------------------------------------------------------------- the footer below the composer

COLORREF CFooterBar::Background() const
{
    return RGB(255, 255, 255);
}

void CFooterBar::SetStatus(const std::wstring& status)
{
    status_ = status;
    Invalidate(FALSE);
}

void CFooterBar::SetModel(const std::wstring& model, const std::wstring& effort, bool enabled)
{
    model_ = model;
    effort_ = effort;
    enabled_ = enabled;
    Invalidate(FALSE);
}

void CFooterBar::SetHealth(Health h)
{
    health_ = h;
    Invalidate(FALSE);
}

void CFooterBar::Draw(CDC& dc, const CRect& rc)
{
    parts_.clear();
    const int radius = Px(8), padX = Px(8), chevronW = Px(16);
    auto width = [&](const std::wstring& s)
    {
        CFont* old = dc.SelectObject(&small_);
        CSize sz = dc.GetTextExtent(s.c_str(), static_cast<int>(s.size()));
        dc.SelectObject(old);
        return static_cast<int>(sz.cx);
    };

    // Right to left: the connection dot, the effort, the model (each opens a menu).
    CRect dot(rc.right - Px(24), Px(2), rc.right, rc.bottom - Px(2));
    if (Hovered(PartStatus))
    {
        Fill(dc, dot, kHover, radius);
    }

    {
        COLORREF color = health_ == Health::Ok ? RGB(22, 163, 74) : health_ == Health::Failed ? RGB(220, 38, 38)
                                                                                              : kMuted;
        int d = Px(8);
        CPoint c = dot.CenterPoint();
        CBrush b(color);
        CPen pen(PS_SOLID, 1, color);
        CBrush* ob = dc.SelectObject(&b);
        CPen* op = dc.SelectObject(&pen);
        dc.Ellipse(c.x - d / 2, c.y - d / 2, c.x + d / 2 + 1, c.y + d / 2 + 1);
        dc.SelectObject(ob);
        dc.SelectObject(op);
    }

    AddHit(dot, PartStatus);
    parts_[PartStatus] = dot;
    int right = dot.left - Px(2);

    auto part = [&](int id, const std::wstring& text, bool chevron, COLORREF color)
    {
        int w = width(text) + padX * 2 + (chevron ? chevronW : 0);
        CRect r(std::max<int>(rc.left + rc.Width() / 3, right - w), Px(2), right, rc.bottom - Px(2));
        if (enabled_ && Hovered(id))
        {
            Fill(dc, r, kHover, radius);
        }

        Text(dc, text, CRect(r.left + padX, r.top, r.right - padX - (chevron ? chevronW : 0), r.bottom),
            enabled_ ? color : kMuted, small_);
        if (chevron)
        {
            Icon(dc, kChevronDown, CRect(r.right - padX - chevronW, r.top, r.right - padX + Px(2), r.bottom), kMuted);
        }

        if (enabled_)
        {
            AddHit(r, id);
        }

        parts_[id] = r;
        right = r.left - Px(2);
    };

    if (!effort_.empty())
    {
        part(PartEffort, effort_, false, kMuted);
    }

    part(PartModel, model_, true, kText);
    Text(dc, status_, CRect(rc.left + Px(4), 0, right - Px(8), rc.bottom), kMuted, small_);
}

void CFooterBar::OnHit(int id, size_t, bool right, CPoint)
{
    if (right || !onClick)
    {
        return;
    }

    CPoint screen(0, 0);
    auto it = parts_.find(id);
    if (it != parts_.end())
    {
        screen = it->second.TopLeft();
        ClientToScreen(&screen);
    }

    onClick(id, screen);
}

// ---------------------------------------------------------------- the composer's buttons

void DrawIconButton(const DRAWITEMSTRUCT& di, wchar_t glyph, bool filled, bool hot)
{
    CDC* dc = CDC::FromHandle(di.hDC);
    CRect rc = di.rcItem;
    int dpi = static_cast<int>(GetDpiForWindow(di.hwndItem));
    bool disabled = (di.itemState & ODS_DISABLED) != 0, pressed = (di.itemState & ODS_SELECTED) != 0;
    const COLORREF white = RGB(255, 255, 255);
    dc->FillSolidRect(rc, white);
    COLORREF back = filled ? (disabled ? RGB(200, 200, 205) : pressed ? RGB(10, 10, 12)
                                                                      : RGB(38, 38, 42))
                           : (pressed ? kSelected : hot ? kHover
                                                        : white);
    COLORREF fore = filled ? white : (disabled ? RGB(180, 180, 185) : RGB(60, 60, 66));
    if (back != white)
    {
        CBrush b(back);
        CPen pen(PS_SOLID, 1, back);
        CBrush* ob = dc->SelectObject(&b);
        CPen* op = dc->SelectObject(&pen);
        dc->Ellipse(rc);
        dc->SelectObject(ob);
        dc->SelectObject(op);
    }

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(filled ? 10 : 12, dpi, 72);
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, IconFace());
    CFont font;
    font.CreateFontIndirectW(&lf);
    CFont* old = dc->SelectObject(&font);
    dc->SetBkMode(TRANSPARENT);
    dc->SetTextColor(fore);
    wchar_t s[2] = { glyph, 0 };
    dc->DrawText(s, 1, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    dc->SelectObject(old);
}

// ---------------------------------------------------------------- Git

std::wstring GitBranch(const std::wstring& folder)
{
    std::error_code ec;
    for (fs::path dir = folder; !dir.empty(); dir = dir.parent_path())
    {
        fs::path git = dir / L".git";
        fs::path head;
        if (fs::is_directory(git, ec))
        {
            head = git / L"HEAD";
        }
        else if (fs::is_regular_file(git, ec))
        {
            // A worktree: ".git" holds "gitdir: <path>".
            std::string text;
            if (ReadFileBytes(git.wstring(), text, 4096) && text.rfind("gitdir:", 0) == 0)
            {
                fs::path gitdir = Wide(TrimCopy(text.substr(7)));
                head = (gitdir.is_relative() ? dir / gitdir : gitdir) / L"HEAD";
            }
        }

        if (!head.empty())
        {
            std::string text;
            if (!ReadFileBytes(head.wstring(), text, 4096))
            {
                return {};
            }

            text = TrimCopy(text);
            if (text.rfind("ref: refs/heads/", 0) == 0)
            {
                return Wide(text.substr(16));
            }

            return Wide(text.substr(0, 7)) + L" (detached)";
        }

        if (dir == dir.root_path())
        {
            break;
        }
    }

    return {};
}

}  // namespace chat
