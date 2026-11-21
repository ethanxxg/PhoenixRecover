#include "WinWicImageDecoder.h"

#include <QtGlobal>
#include <QtCore/QFileInfo>
#include <QtGui/QImage>

#if !defined(Q_OS_WIN)

namespace WinWicImageDecoder {

bool loadFileToImage(const QString & /*localFilePath*/, QImage *outImage)
{
    if (outImage) {
        *outImage = QImage();
    }
    return false;
}

} // namespace WinWicImageDecoder

#else

#include <QByteArray>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <objbase.h>

namespace {

constexpr UINT kMaxDecodeEdge = 8192u;

static bool createScaledConverter(IWICImagingFactory *factory,
                                  IWICBitmapSource *source,
                                  UINT targetW,
                                  UINT targetH,
                                  IWICFormatConverter **outConverter)
{
    IWICBitmapScaler *scaler = nullptr;
    HRESULT hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr) || !scaler) {
        return false;
    }
    hr = scaler->Initialize(source, targetW, targetH, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) {
        scaler->Release();
        return false;
    }

    IWICFormatConverter *conv = nullptr;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv) {
        scaler->Release();
        return false;
    }
    hr = conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                          WICBitmapPaletteTypeMedianCut);
    scaler->Release();
    if (FAILED(hr)) {
        conv->Release();
        return false;
    }
    *outConverter = conv;
    return true;
}

} // namespace

namespace WinWicImageDecoder {

bool loadFileToImage(const QString &localFilePath, QImage *outImage)
{
    if (!outImage || localFilePath.isEmpty()) {
        return false;
    }
    *outImage = QImage();

    const QString absPath = QFileInfo(localFilePath).absoluteFilePath();
    if (absPath.isEmpty() || !QFileInfo::exists(absPath)) {
        return false;
    }

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (hrCom == S_OK);
    if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) {
        return false;
    }

    IWICImagingFactory *factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, reinterpret_cast<void **>(&factory));
    if (FAILED(hr) || !factory) {
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    IWICBitmapDecoder *decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(reinterpret_cast<const WCHAR *>(absPath.utf16()), nullptr,
                                            GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        factory->Release();
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    IWICBitmapFrameDecode *frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        decoder->Release();
        factory->Release();
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    UINT w = 0;
    UINT h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        frame->Release();
        decoder->Release();
        factory->Release();
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    IWICFormatConverter *converter = nullptr;
    UINT outW = w;
    UINT outH = h;
    if (w > kMaxDecodeEdge || h > kMaxDecodeEdge) {
        const double scale =
            static_cast<double>(kMaxDecodeEdge) / static_cast<double>(w > h ? w : h);
        outW = static_cast<UINT>(static_cast<double>(w) * scale);
        outH = static_cast<UINT>(static_cast<double>(h) * scale);
        if (outW < 1) {
            outW = 1;
        }
        if (outH < 1) {
            outH = 1;
        }
        if (!createScaledConverter(factory, frame, outW, outH, &converter)) {
            frame->Release();
            decoder->Release();
            factory->Release();
            if (needUninit) {
                CoUninitialize();
            }
            return false;
        }
    } else {
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr) || !converter) {
            frame->Release();
            decoder->Release();
            factory->Release();
            if (needUninit) {
                CoUninitialize();
            }
            return false;
        }
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr)) {
            converter->Release();
            frame->Release();
            decoder->Release();
            factory->Release();
            if (needUninit) {
                CoUninitialize();
            }
            return false;
        }
    }

    UINT cw = 0;
    UINT ch = 0;
    converter->GetSize(&cw, &ch);
    const UINT stride = cw * 4u;
    const size_t bufBytes = static_cast<size_t>(stride) * static_cast<size_t>(ch);
    QByteArray buf(static_cast<int>(bufBytes), '\0');
    WICRect rect{0, 0, static_cast<INT>(cw), static_cast<INT>(ch)};
    hr = converter->CopyPixels(&rect, stride, static_cast<UINT>(bufBytes),
                                reinterpret_cast<BYTE *>(buf.data()));
    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    if (needUninit) {
        CoUninitialize();
    }
    if (FAILED(hr)) {
        return false;
    }

    QImage img(reinterpret_cast<const uchar *>(buf.constData()), static_cast<int>(cw),
               static_cast<int>(ch), static_cast<int>(stride), QImage::Format_ARGB32);
    *outImage = img.copy();
    return !outImage->isNull();
}

} // namespace WinWicImageDecoder

#endif // Q_OS_WIN
