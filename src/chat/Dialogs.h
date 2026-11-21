#pragma once
#include "stdafx.h"

#include "Mcp.h"
#include "Store.h"
#include "resource.h"

namespace chat
{

class CSystemMessageDlg : public CDialogEx
{
public:
    CSystemMessageDlg(CWnd* parent, const std::string& text);
    std::string text;
    bool makeDefault = false;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
};

// Check boxes per tool: Built-in, then one group per connected MCP server.
class CToolsDlg : public CDialogEx
{
public:
    struct Group
    {
        std::string label;
        std::vector<std::pair<std::string, std::string>> tools;  // qualified name, label
    };
    CToolsDlg(CWnd* parent, std::vector<Group> groups, std::set<std::string> disabled);
    std::set<std::string> disabled;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnTreeClick(NMHDR* nm, LRESULT* result);
    afx_msg void OnTreeKey(NMHDR* nm, LRESULT* result);
    afx_msg LRESULT OnTreeCheck(WPARAM, LPARAM item);
    afx_msg void OnAll();
    afx_msg void OnNone();
    DECLARE_MESSAGE_MAP()

private:
    void SetAll(BOOL check);
    void UpdateGroupLabel(HTREEITEM group);
    CTreeCtrl tree_;
    std::vector<Group> groups_;
    std::vector<std::string> names_;  // item data: index into names_
};

class CMcpEditDlg : public CDialogEx
{
public:
    CMcpEditDlg(CWnd* parent, McpServerConfig cfg);
    McpServerConfig cfg;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnTransport();
    DECLARE_MESSAGE_MAP()
};

class CMcpServersDlg : public CDialogEx
{
public:
    CMcpServersDlg(CWnd* parent, std::vector<McpServerConfig> configs, McpManager& manager);
    std::vector<McpServerConfig> configs;
    bool reconnect = false;

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnAdd();
    afx_msg void OnEdit();
    afx_msg void OnRemove();
    afx_msg void OnReconnect();
    afx_msg void OnDblClk(NMHDR*, LRESULT*);
    afx_msg LRESULT OnMcpChanged(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()

private:
    void Fill();
    CListCtrl list_;
    McpManager& manager_;
};

class CSettingsDlg : public CDialogEx
{
public:
    CSettingsDlg(CWnd* parent, const Settings& s);
    Settings s;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnBackend();
    afx_msg void OnBrowse();
    afx_msg void OnOpenManager();
    afx_msg void OnDataFolder();
    DECLARE_MESSAGE_MAP()
};

class CRenameDlg : public CDialogEx
{
public:
    CRenameDlg(CWnd* parent, const std::string& text);
    std::string text;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
};

class CAutoDlg : public CDialogEx
{
public:
    CAutoDlg(CWnd* parent, const Settings& s, std::vector<std::string> models);
    Settings s;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;

private:
    std::vector<std::string> models_;
};

class CProviderEditDlg : public CDialogEx
{
public:
    CProviderEditDlg(CWnd* parent, ProviderConfig p, bool isNew);
    ProviderConfig p;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnPreset();
    afx_msg void OnKind();
    afx_msg void OnTest();
    afx_msg void OnKeyPage();
    afx_msg void OnSignIn();  // gh auth login --hostname <host>, or ant auth login, in a console window
    DECLARE_MESSAGE_MAP()

private:
    bool Read(ProviderConfig& out);
    void UpdateNotes();
    bool isNew_;
    std::string keyPage_;
};

class CProvidersDlg : public CDialogEx
{
public:
    CProvidersDlg(CWnd* parent, std::vector<ProviderConfig> providers);
    std::vector<ProviderConfig> providers;
    bool autoImport = true;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnAdd();
    afx_msg void OnEdit();
    afx_msg void OnRemove();
    afx_msg void OnDetect();
    afx_msg void OnDblClk(NMHDR*, LRESULT*);
    DECLARE_MESSAGE_MAP()

private:
    void Fill();
    std::string UniqueId(std::string base, int skip = -1) const;
    CListCtrl list_;
};

// ModelHubManager.exe next to this program, or the one install.json names; empty when not found.
std::wstring FindManagerExe();

}  // namespace chat
