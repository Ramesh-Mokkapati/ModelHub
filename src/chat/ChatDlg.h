#pragma once
#include "stdafx.h"

#include "Http.h"
#include "LlmClient.h"
#include "Mcp.h"
#include "OllamaMigrate.h"
#include "Panels.h"
#include "Store.h"
#include "resource.h"

namespace chat
{

struct Attachment
{
    enum class Kind
    {
        Text,
        Image,
        Resource
    } kind = Kind::Text;
    std::string name;     // file path or resource URI
    std::string content;  // text, or the image bytes
    std::string mime;
};

// A multi-line edit that shows a hint while it is empty (EM_SETCUEBANNER works only on single-line edits).
class CCueEdit : public CEdit
{
public:
    CString cue;
    std::function<bool()> onPaste;  // true: the paste was handled (an image, files, long text)

protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnChange();
    afx_msg LRESULT OnPasteMessage(WPARAM, LPARAM);  // WM_PASTE: the context menu's Paste
    DECLARE_MESSAGE_MAP()
};

// The transcript: clicks on "buttons" (copy, download, URLs) are hit-tested here, because rich edit 8 does not
// keep the link effect on text that is not a URL.
class CTranscriptCtrl : public CRichEditCtrl
{
public:
    std::function<bool(long)> isLink;   // a character index -> is it part of a button
    std::function<void(long)> onClick;
    long HitChar(CPoint pt);            // -1: not over text

protected:
    afx_msg BOOL OnSetCursor(CWnd* wnd, UINT hitTest, UINT message);
    afx_msg void OnLButtonDown(UINT flags, CPoint pt);
    afx_msg void OnLButtonUp(UINT flags, CPoint pt);
    DECLARE_MESSAGE_MAP()

private:
    long downChar_ = -1;
};

class CChatDlg : public CDialogEx
{
public:
    CChatDlg();
    ~CChatDlg() override;

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG* msg) override;
    void OnOK() override;
    void OnCancel() override;

    afx_msg void OnSize(UINT type, int cx, int cy);
    // Dragging the edges between the panels and the transcript.
    afx_msg void OnLButtonDown(UINT flags, CPoint pt);
    afx_msg void OnMouseMove(UINT flags, CPoint pt);
    afx_msg void OnLButtonUp(UINT flags, CPoint pt);
    afx_msg BOOL OnSetCursor(CWnd* wnd, UINT hit, UINT msg);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* mmi);
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT di);
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT ctlColor);
    afx_msg void OnClose();
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnDestroy();
    afx_msg void OnDropFiles(HDROP drop);
    afx_msg void OnNewChat();
    afx_msg void OnSearchChange();
    afx_msg void OnMcpServers();
    afx_msg void OnProviders();
    afx_msg LRESULT OnImportProviders(WPARAM, LPARAM);
    afx_msg LRESULT OnMigrated(WPARAM, LPARAM);
    afx_msg void OnSettings();
    afx_msg void OnWorkspaceChange();
    afx_msg void OnClearAttachments();
    afx_msg void OnPlus();
    afx_msg void OnMic();
    afx_msg void OnInputChange();
    afx_msg void OnModelChange();
    afx_msg void OnRefreshModels();
    afx_msg void OnSend();
    afx_msg void OnTreeSelChanged(NMHDR* nm, LRESULT* result);
    afx_msg void OnTreeRClick(NMHDR* nm, LRESULT* result);
    void OnTranscriptClick(long cp);
    afx_msg LRESULT OnAppEvent(WPARAM, LPARAM);
    afx_msg LRESULT OnModels(WPARAM, LPARAM);
    afx_msg LRESULT OnMcpChanged(WPARAM, LPARAM);
    afx_msg LRESULT OnConfirm(WPARAM, LPARAM);
    afx_msg LRESULT OnDownloadProgress(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()

private:
    // layout and look
    void CreateFonts();
    void Layout();
    int Px(int v) const;

    // conversations
    std::shared_ptr<Conversation> NewConversation();
    void OpenConversation(const std::shared_ptr<Conversation>& c);
    void FillTree();
    void SaveCurrent();
    std::wstring WorkspaceOf(const Conversation& c) const;
    void UpdateWorkspaceLabel();  // the right panel and the title strip
    void UpdateInfoPanel();
    void SelectConversation(size_t index);
    void ShowConversationMenu(const std::shared_ptr<Conversation>& c, CPoint screen);
    int SplitterAt(CPoint pt) const;
    // The toolbar above the conversation and the bar below the composer.
    void OnToolbar(int button, CPoint screen);
    void ShowModelMenu(CPoint screen);
    void ShowEffortMenu(CPoint screen);
    void OpenTerminal();
    void UpdateFooter();  // 1: the left panel's edge, 2: the right panel's, 0: neither
    void ExportConversation(const Conversation& c);

    // transcript
    struct Style
    {
        COLORREF color = RGB(32, 33, 36);
        bool bold = false;
        bool italic = false;
        bool code = false;
        int size = 0;  // points; 0 = body size
        COLORREF back = CLR_NONE;
        bool link = false;
    };
    // Clickable text in the transcript: copy a code block or an answer, open a URL, download a model.
    enum class LinkKind
    {
        Copy,
        Url,
        Download
    };
    struct LinkTarget
    {
        long cpMin = 0, cpMax = 0;
        LinkKind kind = LinkKind::Copy;
        std::wstring payload;  // the text to copy, the URL or the model id
    };
    long EndPos();
    size_t AddLink(const std::wstring& label, LinkKind kind, const std::wstring& payload, Style style);
    void AppendRoute(const json& m);
    void CopyToClipboard(const std::wstring& text, const char* what);
    void StartDownload(const std::string& id, unsigned long long size);
    void ShowAutoDialog();
    void Append(const std::wstring& text, const Style& style);
    void AppendMarkdown(const std::string& text);
    void AppendInline(const std::wstring& line, const Style& base);
    void AppendMessage(const json& m);
    void RenderConversation();
    void ScrollToEnd();
    void UpdateEmptyState();

    // composer
    void AddAttachment(const std::wstring& path);
    void UpdateAttachments();
    bool PasteFromClipboard();  // false: plain text for the edit box itself
    // Ollama's models into ModelHub (asks first; fromMenu: also when nothing was declined) and Claude's setup.
    void OfferOllamaMove(const OllamaInventory& inv, bool fromMenu);
    void OfferClaudeSetup(bool fromMenu);
    void EnsureClaudeProvider();
    void SetBusy(bool busy);
    void SetStatus(const std::string& text);
    void RefreshModels();
    void ApplyMcp();
    void ShowToolsDialog();
    void AttachResource(const std::shared_ptr<McpServer>& server, const McpResource& r);

    Settings settings_;
    McpManager mcp_;
    std::vector<std::shared_ptr<Conversation>> conversations_;  // newest first
    std::shared_ptr<Conversation> current_;
    std::vector<std::shared_ptr<Conversation>> treeItems_;
    bool fillingTree_ = false;
    std::vector<Attachment> attachments_;
    std::vector<LinkTarget> links_;
    std::vector<std::string> models_;       // chat models: ModelHub's, then "<provider>|<model>"
    std::vector<std::string> comboIds_;     // the model list's entries: "auto", then models_
    std::set<std::string> downloading_;
    json pendingRoute_;                     // Auto mode's choice for the answer being written
    json pendingToolNotes_ = json::array(); // tool calls Copilot made for the answer being written
    void AppendToolNote(const json& note);

    std::thread worker_;
    std::shared_ptr<CancelToken> cancel_;
    bool busy_ = false;
    int streamKind_ = 0;  // 0: nothing yet, 1: reasoning, 2: content
    int modelsRequest_ = 0;
    int pasteCount_ = 0;
    std::string importNote_;  // "Imported ...": shown with the next model list status

    CFont fontTitle_, fontHello_, fontHelloSub_, fontInput_, fontSidebar_;
    CBrush sidebarBrush_, whiteBrush_;
    CSidebarPanel sidebar_;
    CInfoPanel info_;
    CTitleStrip titleStrip_;
    CFooterBar footer_;
    CRect composerRect_, sidebarEdge_, infoEdge_;
    int dragging_ = 0;  // SplitterAt() of the edge being dragged
    CTranscriptCtrl transcript_;
    CTreeCtrl tree_;
    CCueEdit input_;
    CButton mic_;
    int inputLines_ = 1;
    std::wstring waitStatus_;  // "Waiting for ...": the footer adds the seconds so far
    ULONGLONG waitSince_ = 0;  // the message box grows with its text, up to 8 lines
    CEdit search_;
    CComboBox model_;
    HICON icon_ = nullptr;
};

}  // namespace chat
