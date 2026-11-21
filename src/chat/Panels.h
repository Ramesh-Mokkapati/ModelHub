#pragma once
#include "stdafx.h"

// The window's side panels, drawn by hand in the style of Claude's desktop app:
//   CSidebarPanel  left: New chat, Search, MCP Servers, Model Providers, Settings; conversations by project; the user
//   CInfoPanel     right: Environment (computer, folder, Git branch, model), Context (window use), Changes (files)
//   CTitleStrip    above the transcript: the conversation's title and the buttons that show or hide the panels
// Icons come from Windows' icon font (Segoe Fluent Icons, else Segoe MDL2 Assets).

namespace chat
{

// What every panel shares: DPI-scaled fonts, double-buffered painting, hover and click areas.
class CPanelBase : public CWnd
{
public:
    BOOL CreatePanel(CWnd* parent, UINT id);
    int Px(int v) const;

protected:
    struct Hit
    {
        CRect rc;
        int id = 0;
        size_t data = 0;
    };
    virtual void Draw(CDC& dc, const CRect& rc) = 0;
    virtual void OnHit(int id, size_t data, bool right, CPoint screen) = 0;
    virtual COLORREF Background() const = 0;
    virtual void OnScroll(int /*delta*/)
    {
    }
    void AddHit(const CRect& rc, int id, size_t data = 0);
    bool Hovered(int id, size_t data = 0) const;
    void Icon(CDC& dc, wchar_t glyph, const CRect& rc, COLORREF color, UINT format = DT_CENTER);
    void Text(CDC& dc, const std::wstring& s, CRect rc, COLORREF color, CFont& font, UINT format = DT_LEFT);
    void Fill(CDC& dc, const CRect& rc, COLORREF color, int radius = 0);
    void EnsureFonts();

    CFont font_, small_, bold_, title_, icon_;
    UINT fontDpi_ = 0;
    std::vector<Hit> hits_;
    int hoverId_ = -1;
    size_t hoverData_ = 0;
    bool tracking_ = false;

    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC*);
    afx_msg void OnMouseMove(UINT, CPoint pt);
    afx_msg LRESULT OnMouseLeave(WPARAM, LPARAM);
    afx_msg void OnLButtonUp(UINT, CPoint pt);
    afx_msg void OnRButtonUp(UINT, CPoint pt);
    afx_msg BOOL OnMouseWheel(UINT, short delta, CPoint);
    afx_msg BOOL OnSetCursor(CWnd*, UINT, UINT);
    DECLARE_MESSAGE_MAP()

private:
    const Hit* HitAt(CPoint pt) const;
};

class CSidebarPanel : public CPanelBase
{
public:
    enum Nav
    {
        NavNewChat = 1,
        NavMcp,
        NavProviders,
        NavSettings,
        NavCollapse,
        NavGroup,         // data: group index (collapse or expand)
        NavConversation,  // data: conversation index
    };
    struct Conv
    {
        std::wstring title;
        size_t index = 0;  // the caller's
        bool selected = false;
    };
    struct Group
    {
        std::wstring key, label;
        std::vector<Conv> items;
    };
    void SetGroups(std::vector<Group> groups);
    void SetMcpLabel(const std::wstring& label);
    std::wstring Filter() const;
    HWND SearchHwnd() const
    {
        return search_.m_hWnd;
    }
    std::function<void(int nav, size_t data, bool right, CPoint screen)> onNav;
    std::function<void()> onSearch;

protected:
    void Draw(CDC& dc, const CRect& rc) override;
    void OnHit(int id, size_t data, bool right, CPoint screen) override;
    COLORREF Background() const override;
    void OnScroll(int delta) override;
    afx_msg int OnCreate(LPCREATESTRUCT cs);
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT type);
    afx_msg void OnSearchChange();
    DECLARE_MESSAGE_MAP()

private:
    CEdit search_;
    CBrush searchBrush_;
    std::vector<Group> groups_;
    std::set<std::wstring> collapsed_;
    std::wstring mcpLabel_ = L"MCP Servers";
    std::wstring user_;
    int scroll_ = 0, contentHeight_ = 0, listTop_ = 0, listBottom_ = 0;
};

class CInfoPanel : public CPanelBase
{
public:
    enum Action
    {
        ActChangeFolder = 1,
        ActOpenFolder,
        ActToggleContext,
        ActOpenChange,  // data: change index
        ActClose,
    };
    struct Data
    {
        std::wstring computer, folderName, folderPath, branch, model, service;
        int contextPercent = 0;
        std::wstring contextSummary;  // "12.3k of 128k tokens"
        std::vector<std::pair<std::wstring, std::wstring>> contextDetails;
        std::vector<std::pair<std::wstring, std::wstring>> changes;  // label, full path
    };
    void SetData(Data d);
    const Data& GetData() const
    {
        return data_;
    }
    std::function<void(int action, size_t data)> onAction;

protected:
    void Draw(CDC& dc, const CRect& rc) override;
    void OnHit(int id, size_t data, bool right, CPoint screen) override;
    COLORREF Background() const override;

private:
    Data data_;
    bool contextOpen_ = false;
};

class CTitleStrip : public CPanelBase
{
public:
    enum Button
    {
        BtnShowSidebar = 1,
        BtnToggleInfo,
        BtnTools,
        BtnTerminal,
        BtnFolder,
        BtnMore,
    };
    void SetState(const std::wstring& title, bool sidebarVisible, bool infoVisible);
    std::function<void(int button, CPoint screen)> onButton;  // screen: below the button, for a menu

protected:
    void Draw(CDC& dc, const CRect& rc) override;
    void OnHit(int id, size_t data, bool right, CPoint screen) override;
    COLORREF Background() const override;

private:
    BOOL PreTranslateMessage(MSG* msg) override;
    std::wstring title_;
    bool sidebar_ = true, info_ = true;
    std::map<int, CRect> buttons_;
    CToolTipCtrl tips_;
};

// Below the composer: the status on the left; the model, the thinking effort and the connection on the right.
class CFooterBar : public CPanelBase
{
public:
    enum Part
    {
        PartModel = 1,
        PartEffort,
        PartStatus,
    };
    enum class Health
    {
        Unknown,
        Ok,
        Failed
    };
    void SetStatus(const std::wstring& status);
    void SetModel(const std::wstring& model, const std::wstring& effort, bool enabled);
    void SetHealth(Health h);
    std::function<void(int part, CPoint screen)> onClick;  // screen: the part's top-left corner

protected:
    void Draw(CDC& dc, const CRect& rc) override;
    void OnHit(int id, size_t data, bool right, CPoint screen) override;
    COLORREF Background() const override;

private:
    std::wstring status_, model_, effort_;
    bool enabled_ = true;
    Health health_ = Health::Unknown;
    std::map<int, CRect> parts_;
};

// Owner-drawn round icon buttons of the composer: "+", Send and the microphone (plain), Stop (filled).
void DrawIconButton(const DRAWITEMSTRUCT& di, wchar_t glyph, bool filled, bool hot);
constexpr wchar_t kGlyphAdd = 0xE710, kGlyphSend = 0xE751, kGlyphStop = 0xE71A, kGlyphMic = 0xE720;

// Git branch of the folder (or a parent): "main", "a1b2c3d (detached)", or "" when it is not in a repository.
std::wstring GitBranch(const std::wstring& folder);

}  // namespace chat
