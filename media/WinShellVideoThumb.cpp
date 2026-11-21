#include "WinShellVideoThumb.h"

#include <QtGlobal>
#include <QtCore/QDir>
#include <QtGui/QImage>
#include <QtGui/QPixmap>

#if !defined(Q_OS_WIN)

namespace WinShellVideoThumb {

QPixmap thumbnailForFile(const QString & /*localPath*/)
{
    return {};
}

} // namespace WinShellVideoThumb

#else

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <objbase.h>

namespace {

static QImage qImageFromHBitmapDdb(HBITMAP hBitmap)
{
    if (!hBitmap) {
        return QImage();
    }
    BITMAP bm{};
    if (GetObject(hBitmap, sizeof(bm), &bm) != sizeof(bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return QImage();
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
    if (!hdcSrc) {
        ReleaseDC(nullptr, hdcScreen);
        return QImage();
    }
    const HGDIOBJ oldSrc = SelectObject(hdcSrc, hBitmap);

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = bm.bmWidth;
    bih.biHeight = -bm.bmHeight;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    BITMAPINFO biWrap{};
    biWrap.bmiHeader = bih;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        SelectObject(hdcSrc, oldSrc);
        DeleteDC(hdcSrc);
        ReleaseDC(nullptr, hdcScreen);
        return QImage();
    }
    void *dibBits = nullptr;
    HBITMAP hDib = CreateDIBSection(hdcMem, &biWrap, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    QImage out;
    if (hDib && dibBits) {
        const HGDIOBJ oldMem = SelectObject(hdcMem, hDib);
        BitBlt(hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, hdcSrc, 0, 0, SRCCOPY);
        SelectObject(hdcMem, oldMem);
        out = QImage(bm.bmWidth, bm.bmHeight, QImage::Format_ARGB32);
        std::memcpy(out.bits(), dibBits, static_cast<size_t>(out.sizeInBytes()));
        DeleteObject(hDib);
    }

    DeleteDC(hdcMem);
    SelectObject(hdcSrc, oldSrc);
    DeleteDC(hdcSrc);
    ReleaseDC(nullptr, hdcScreen);
    return out;
}

} // namespace

namespace WinShellVideoThumb {

QPixmap thumbnailForFile(const QString &localPath)
{
    const QString native = QDir::toNativeSeparators(localPath);
    if (native.isEmpty()) {
        return {};
    }

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (hrCom == S_OK);
    if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) {
        return {};
    }

    IShellItemImageFactory *factory = nullptr;
    const HRESULT hr = SHCreateItemFromParsingName(
        reinterpret_cast<PCWSTR>(native.utf16()),
        nullptr,
        IID_IShellItemImageFactory,
        reinterpret_cast<void **>(&factory));

    QPixmap result;
    if (SUCCEEDED(hr) && factory) {
        SIZE req{};
        req.cx = 320;
        req.cy = 240;
        const SIIGBF flagsTry[] = {
            static_cast<SIIGBF>(SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK),
            static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK),
            static_cast<SIIGBF>(0),
        };
        for (SIIGBF flg : flagsTry) {
            HBITMAP hb = nullptr;
            const HRESULT hrImg = factory->GetImage(req, flg, &hb);
            if (SUCCEEDED(hrImg) && hb) {
                const QImage im = qImageFromHBitmapDdb(hb);
                DeleteObject(hb);
                if (!im.isNull()) {
                    result = QPixmap::fromImage(im);
                    break;
                }
            }
        }
        factory->Release();
    }

    if (needUninit) {
        CoUninitialize();
    }
    return result;
}

} // namespace WinShellVideoThumb

#endif // Q_OS_WIN
