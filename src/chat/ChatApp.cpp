#include "stdafx.h"

#include "ChatDlg.h"

namespace
{

class CChatApp : public CWinApp
{
public:
    BOOL InitInstance() override
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
        CWinApp::InitInstance();
        if (!AfxInitRichEdit5())
        {
            AfxMessageBox(L"The rich edit control (Msftedit.dll) is not available.", MB_ICONERROR);
            return FALSE;
        }

        SetRegistryKey(L"ModelHub");
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC: pasted images
        {
            chat::CChatDlg dlg;
            m_pMainWnd = &dlg;
            dlg.DoModal();
        }

        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }

        return FALSE;  // the dialog closed: end the application
    }
};

CChatApp theApp;

}  // namespace
