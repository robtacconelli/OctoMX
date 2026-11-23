// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] Windows Defender Firewall check (see win_firewall.h). Read only.
#include "core/diag/win_firewall.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>
#endif

namespace mx::diag {

#ifndef Q_OS_WIN
FirewallState checkWindowsFirewall(const QString &programPath)
{
    Q_UNUSED(programPath)
    FirewallState s;
    s.error = QStringLiteral("not checked on this system");
    return s;
}
#else
namespace {
// {E2B3C97F-6AE1-41AC-817A-F6F92166D7DD}
const CLSID kClsidPolicy2 = {0xE2B3C97F, 0x6AE1, 0x41AC, {0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD}};
// {98325047-C671-4174-8D81-DEFCD3F03186}
const IID kIidPolicy2 = {0x98325047, 0xC671, 0x4174, {0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86}};
// {AF230D27-BABA-4E42-ACED-F524F22CFCE2}
const IID kIidRule = {0xAF230D27, 0xBABA, 0x4E42, {0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2}};
// {00020404-0000-0000-C000-000000000046}
const IID kIidEnumVariant = {0x00020404, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

QString bstr(BSTR b) { return b ? QString::fromWCharArray(b, int(SysStringLen(b))) : QString(); }
}

FirewallState checkWindowsFirewall(const QString &programPath)
{
    FirewallState s;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    INetFwPolicy2 *pol = nullptr;
    HRESULT hr = CoCreateInstance(kClsidPolicy2, nullptr, CLSCTX_INPROC_SERVER, kIidPolicy2, reinterpret_cast<void **>(&pol));
    if (FAILED(hr) || !pol) {
        s.error = QStringLiteral("the firewall service could not be asked (error 0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'));
        if (SUCCEEDED(ci)) CoUninitialize();
        return s;
    }
    long types = 0;
    pol->get_CurrentProfileTypes(&types);
    QStringList names;
    const NET_FW_PROFILE_TYPE2 all[] = {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC};
    const char *labels[] = {"Domain", "Private", "Public"};
    for (int i = 0; i < 3; ++i) {
        if (!(types & all[i])) continue;
        names << QLatin1String(labels[i]);
        VARIANT_BOOL on = VARIANT_FALSE, block = VARIANT_FALSE;
        NET_FW_ACTION inAction = NET_FW_ACTION_BLOCK;
        pol->get_FirewallEnabled(all[i], &on);
        pol->get_BlockAllInboundTraffic(all[i], &block);
        pol->get_DefaultInboundAction(all[i], &inAction);
        if (on == VARIANT_TRUE) s.enabled = true;
        if (on == VARIANT_TRUE && block == VARIANT_TRUE) s.blockAllInbound = true;
        if (inAction == NET_FW_ACTION_ALLOW) s.inboundDefaultBlock = false;
    }
    s.profiles = names.join(QStringLiteral(", "));
    INetFwRules *rules = nullptr;
    if (SUCCEEDED(pol->get_Rules(&rules)) && rules) {
        IUnknown *unk = nullptr;
        if (SUCCEEDED(rules->get__NewEnum(&unk)) && unk) {
            IEnumVARIANT *en = nullptr;
            if (SUCCEEDED(unk->QueryInterface(kIidEnumVariant, reinterpret_cast<void **>(&en))) && en) {
                const QString want = QDir::toNativeSeparators(programPath).toLower();
                // [ren-1] the legacy program file in the same folder (its rules do not cover octomx.exe)
                const QString old = QDir::toNativeSeparators(QFileInfo(programPath).absolutePath() + QStringLiteral("/winmx.exe")).toLower();
                VARIANT v;
                VariantInit(&v);
                ULONG got = 0;
                while (en->Next(1, &v, &got) == S_OK && got == 1) {
                    if (v.vt == VT_DISPATCH && v.pdispVal) {
                        INetFwRule *r = nullptr;
                        if (SUCCEEDED(v.pdispVal->QueryInterface(kIidRule, reinterpret_cast<void **>(&r))) && r) {
                            BSTR app = nullptr;
                            r->get_ApplicationName(&app);
                            if (bstr(app).toLower() == old && old != want) {
                                VARIANT_BOOL on = VARIANT_FALSE;
                                NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
                                NET_FW_ACTION act = NET_FW_ACTION_BLOCK;
                                r->get_Enabled(&on);
                                r->get_Direction(&dir);
                                r->get_Action(&act);
                                if (on == VARIANT_TRUE && dir == NET_FW_RULE_DIR_IN && act == NET_FW_ACTION_ALLOW) {
                                    BSTR name = nullptr;
                                    r->get_Name(&name);
                                    s.oldProgramRules << bstr(name);
                                    SysFreeString(name);
                                }
                            }
                            if (bstr(app).toLower() == want) {
                                VARIANT_BOOL en2 = VARIANT_FALSE;
                                NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
                                NET_FW_ACTION act = NET_FW_ACTION_ALLOW;
                                long prof = 0;
                                r->get_Enabled(&en2);
                                r->get_Direction(&dir);
                                r->get_Action(&act);
                                r->get_Profiles(&prof);
                                if (en2 == VARIANT_TRUE && dir == NET_FW_RULE_DIR_IN && (prof & types)) {
                                    BSTR name = nullptr;
                                    r->get_Name(&name);
                                    s.ruleNames << bstr(name);
                                    SysFreeString(name);
                                    if (act == NET_FW_ACTION_ALLOW) ++s.allowRules;
                                    else ++s.blockRules;
                                }
                            }
                            SysFreeString(app);
                            r->Release();
                        }
                    }
                    VariantClear(&v);
                }
                en->Release();
            }
            unk->Release();
        }
        rules->Release();
    }
    pol->Release();
    s.checked = true;
    if (SUCCEEDED(ci)) CoUninitialize();
    return s;
}
#endif

} // namespace mx::diag
