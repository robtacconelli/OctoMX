// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1 follow-up] Windows UPnP NAT API fallback (see win_upnp.h). llvm-mingw has no natupnp.h:
// the three interfaces used are declared here (vtable order as in the Windows SDK header).
#include "core/diag/win_upnp.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#endif

namespace mx::diag {

#ifndef Q_OS_WIN
static WinUpnpResult notWindows()
{
    WinUpnpResult r;
    r.error = QStringLiteral("only on Windows");
    return r;
}
WinUpnpResult winUpnpProbe() { return notWindows(); }
WinUpnpResult winUpnpAdd(int, const QString &, int, const QString &, const QString &) { return notWindows(); }
WinUpnpResult winUpnpRemove(int, const QString &) { return notWindows(); }
#else
namespace {
struct IStaticPortMapping : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_ExternalIPAddress(BSTR *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExternalPort(long *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InternalPort(long *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Protocol(BSTR *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InternalClient(BSTR *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Enabled(VARIANT_BOOL *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Description(BSTR *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE EditInternalClient(BSTR v) = 0;
    virtual HRESULT STDMETHODCALLTYPE Enable(VARIANT_BOOL v) = 0;
    virtual HRESULT STDMETHODCALLTYPE EditDescription(BSTR v) = 0;
    virtual HRESULT STDMETHODCALLTYPE EditInternalPort(long v) = 0;
};
struct IStaticPortMappingCollection : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown **p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Item(long externalPort, BSTR protocol, IStaticPortMapping **p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Count(long *p) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(long externalPort, BSTR protocol) = 0;
    virtual HRESULT STDMETHODCALLTYPE Add(long externalPort, BSTR protocol, long internalPort, BSTR internalClient,
                                          VARIANT_BOOL enabled, BSTR description, IStaticPortMapping **p) = 0;
};
struct IUPnPNAT : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_StaticPortMappingCollection(IStaticPortMappingCollection **p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DynamicPortMappingCollection(IUnknown **p) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NATEventManager(IUnknown **p) = 0;
};
// {AE1E00AA-3FD5-403C-8A27-2BBDC30CD0E1}
const CLSID kClsidUPnPNAT = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
// {B171C812-CC76-485A-94D8-B6B3A2794E99}
const IID kIidUPnPNAT = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};

QString hr(HRESULT h) { return QStringLiteral("0x%1").arg(quint32(h), 8, 16, QLatin1Char('0')); }
QString str(BSTR b) { return b ? QString::fromWCharArray(b, int(SysStringLen(b))) : QString(); }
BSTR bstr(const QString &s) { return SysAllocString(reinterpret_cast<const OLECHAR *>(s.utf16())); }

// Runs fn with the static mapping collection (or reports why there is none).
template <typename F> WinUpnpResult withCollection(F fn)
{
    WinUpnpResult r;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUPnPNAT *nat = nullptr;
    HRESULT h = CoCreateInstance(kClsidUPnPNAT, nullptr, CLSCTX_ALL, kIidUPnPNAT, reinterpret_cast<void **>(&nat));
    if (FAILED(h) || !nat) {
        r.error = QStringLiteral("the Windows UPnP NAT API is not available (%1)").arg(hr(h));
    } else {
        IStaticPortMappingCollection *col = nullptr;
        h = nat->get_StaticPortMappingCollection(&col);
        if (FAILED(h)) {
            r.error = QStringLiteral("Windows could not ask for a UPnP router (%1)").arg(hr(h));
        } else if (!col) {
            r.error = QStringLiteral("Windows found no UPnP router either");
        } else {
            r.available = true;
            fn(col, r);
            col->Release();
        }
        nat->Release();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return r;
}
} // namespace

WinUpnpResult winUpnpProbe()
{
    return withCollection([](IStaticPortMappingCollection *col, WinUpnpResult &r) {
        long n = -1;
        r.ok = SUCCEEDED(col->get_Count(&n));
        r.count = int(n);
        if (!r.ok) {   // a collection that cannot even count is no router (a stub API)
            r.available = false;
            r.error = QStringLiteral("the Windows UPnP NAT API answered, but cannot list mappings (no usable router)");
        }
    });
}

WinUpnpResult winUpnpAdd(int externalPort, const QString &proto, int internalPort, const QString &internalClient,
                         const QString &description)
{
    return withCollection([&](IStaticPortMappingCollection *col, WinUpnpResult &r) {
        BSTR p = bstr(proto), c = bstr(internalClient), d = bstr(description);
        IStaticPortMapping *m = nullptr;
        const HRESULT h = col->Add(externalPort, p, internalPort, c, VARIANT_TRUE, d, &m);
        if (SUCCEEDED(h) && m) {
            r.ok = true;
            long ext = externalPort;
            m->get_ExternalPort(&ext);
            r.externalPort = int(ext);
            BSTR ip = nullptr;
            if (SUCCEEDED(m->get_ExternalIPAddress(&ip))) r.externalAddress = str(ip);
            SysFreeString(ip);
            m->Release();
        } else {
            r.error = QStringLiteral("the router refused the mapping (%1)").arg(hr(h));
        }
        SysFreeString(p);
        SysFreeString(c);
        SysFreeString(d);
    });
}

WinUpnpResult winUpnpRemove(int externalPort, const QString &proto)
{
    return withCollection([&](IStaticPortMappingCollection *col, WinUpnpResult &r) {
        BSTR p = bstr(proto);
        const HRESULT h = col->Remove(externalPort, p);
        r.ok = SUCCEEDED(h);
        if (!r.ok) r.error = QStringLiteral("removal failed (%1)").arg(hr(h));
        SysFreeString(p);
    });
}
#endif

} // namespace mx::diag
