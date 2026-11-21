#include "Clipboard.h"

#include <atlbase.h>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace chat
{

namespace
{

// Any image WIC can read -> PNG, without alpha: clipboard bitmaps often carry an alpha channel of zeros, which
// would make the picture invisible.
bool ToPng(const BYTE* data, size_t size, std::string& png, UINT& width, UINT& height)
{
    CComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        return false;
    }

    CComPtr<IWICStream> in;
    if (FAILED(factory->CreateStream(&in)) || FAILED(in->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))))
    {
        return false;
    }

    CComPtr<IWICBitmapDecoder> decoder;
    CComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateDecoderFromStream(in, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
    {
        return false;
    }

    frame->GetSize(&width, &height);
    CComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
    {
        return false;
    }

    CComPtr<IStream> out;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &out)))
    {
        return false;
    }

    CComPtr<IWICBitmapEncoder> encoder;
    CComPtr<IWICBitmapFrameEncode> target;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(out, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&target, nullptr)) ||
        FAILED(target->Initialize(nullptr)) || FAILED(target->SetSize(width, height)) ||
        FAILED(target->SetPixelFormat(&format)) || FAILED(target->WriteSource(converter, nullptr)) ||
        FAILED(target->Commit()) || FAILED(encoder->Commit()))
    {
        return false;
    }

    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(out, &mem)))
    {
        return false;
    }

    STATSTG stat{};
    out->Stat(&stat, STATFLAG_NONAME);
    const char* bytes = static_cast<const char*>(GlobalLock(mem));
    if (!bytes)
    {
        return false;
    }

    png.assign(bytes, static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(mem);
    return !png.empty();
}

// CF_DIB is a bitmap without its file header: add one so that WIC's BMP decoder can read it.
bool DibToPng(HGLOBAL dib, std::string& png, UINT& width, UINT& height)
{
    SIZE_T size = GlobalSize(dib);
    const auto* info = static_cast<const BITMAPINFOHEADER*>(GlobalLock(dib));
    if (!info || size < sizeof(BITMAPINFOHEADER))
    {
        if (info)
        {
            GlobalUnlock(dib);
        }

        return false;
    }

    DWORD colors = info->biClrUsed ? info->biClrUsed : (info->biBitCount <= 8 ? 1u << info->biBitCount : 0);
    DWORD masks = info->biSize == sizeof(BITMAPINFOHEADER) && info->biCompression == BI_BITFIELDS ? 12 : 0;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;  // "BM"
    file.bfSize = static_cast<DWORD>(sizeof file + size);
    file.bfOffBits = static_cast<DWORD>(sizeof file + info->biSize + masks + colors * sizeof(RGBQUAD));
    std::string bmp(reinterpret_cast<const char*>(&file), sizeof file);
    bmp.append(reinterpret_cast<const char*>(info), size);
    GlobalUnlock(dib);
    return ToPng(reinterpret_cast<const BYTE*>(bmp.data()), bmp.size(), png, width, height);
}

}  // namespace

ClipboardContent ReadClipboard(HWND owner)
{
    ClipboardContent c;
    if (!OpenClipboard(owner))
    {
        return c;
    }

    static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (HANDLE drop = GetClipboardData(CF_HDROP))
    {
        auto hdrop = static_cast<HDROP>(drop);
        UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i)
        {
            wchar_t path[MAX_PATH * 4];
            if (DragQueryFileW(hdrop, i, path, static_cast<UINT>(std::size(path))))
            {
                c.files.push_back(path);
            }
        }

        if (!c.files.empty())
        {
            c.kind = ClipboardContent::Kind::Files;
        }
    }

    if (c.kind == ClipboardContent::Kind::None && IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        {
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(h)))
            {
                c.text = text;
                GlobalUnlock(h);
            }
        }

        if (!c.text.empty())
        {
            c.kind = ClipboardContent::Kind::Text;
        }
    }

    if (c.kind == ClipboardContent::Kind::None && pngFormat && IsClipboardFormatAvailable(pngFormat))
    {
        if (HANDLE h = GetClipboardData(pngFormat))
        {
            if (const auto* bytes = static_cast<const BYTE*>(GlobalLock(h)))
            {
                bool ok = ToPng(bytes, GlobalSize(h), c.png, c.width, c.height);
                GlobalUnlock(h);
                if (ok)
                {
                    c.kind = ClipboardContent::Kind::Image;
                }
            }
        }
    }

    if (c.kind == ClipboardContent::Kind::None && IsClipboardFormatAvailable(CF_DIB))
    {
        if (HANDLE h = GetClipboardData(CF_DIB))
        {
            if (DibToPng(static_cast<HGLOBAL>(h), c.png, c.width, c.height))
            {
                c.kind = ClipboardContent::Kind::Image;
            }
        }
    }

    CloseClipboard();
    return c;
}

}  // namespace chat
