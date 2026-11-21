#pragma once
#include "stdafx.h"

namespace chat
{

// What a paste brings: files copied in Explorer, an image (a screenshot, a picture copied in a browser or an
// editor), or text. When the clipboard holds both text and a picture of it (Word, Excel), the text wins.
struct ClipboardContent
{
    enum class Kind
    {
        None,
        Files,
        Image,
        Text
    } kind = Kind::None;
    std::vector<std::wstring> files;
    std::string png;  // the image as PNG
    UINT width = 0, height = 0;
    std::wstring text;
};

ClipboardContent ReadClipboard(HWND owner);

}  // namespace chat
