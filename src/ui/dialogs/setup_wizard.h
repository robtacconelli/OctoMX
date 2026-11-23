// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Setup Wizard (UI_SPEC §5.9, reconstructed) [ui-c].
//
// Shown on the first start (before the core starts, so ports and networks apply without a
// restart) and from Settings ▸ "Setup Wizard...". Pages: Welcome (recommended / custom), Screen
// Name, Connection Type, Incoming TCP port (test), UDP port (test), Folders, Networks, Finish.
// The one-click path: "Finish" on the Welcome page with "Recommended settings" selected = every
// network on, automatic port mapping, standard folders, links and .torrent files opened by OctoMX.
#pragma once

#include <QtWidgets/QWizard>

#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QRadioButton;
class QTreeWidget;

namespace mx {
class Core;
}

namespace mx::ui {

class StaticText;
class WrapLabel;
class PathField;

class SetupWizard : public QWizard {
    Q_OBJECT
public:
    enum PageId { Welcome, ScreenName, Connection, TcpPort, UdpPort, Folders, Networks, Finish };

    struct PortTest {
        bool ok = false;
        QString message;
    };
    // Checks a port on this computer (the app provides it: it needs QtNetwork). udp=false: TCP.
    using PortTester = std::function<PortTest(int port, bool udp)>;
    static void setPortTester(const PortTester &t);
    // [diag-1] the real test (asynchronous): the free-port check first, then the router mapping and,
    // for TCP, the public port checker. `report` is called with every step (final = true at the end).
    using PortTestReport = std::function<void(const PortTest &step, bool final)>;
    using RealPortTester = std::function<void(int port, bool udp, bool mapPorts, QObject *context, PortTestReport report)>;
    static void setRealPortTester(const RealPortTester &t);

    explicit SetupWizard(mx::Core *core, QWidget *parent = nullptr);

    bool recommended() const;
    // write everything to Settings (called by accept(); public for tests)
    void applySettings();
    // add the chosen shared folders to the library (needs a started core)
    void applyFolders();
    QStringList sharedFolders() const { return m_folders; }

    void accept() override;
    int nextId() const override;

private:
    QWizardPage *makeWelcome();
    QWizardPage *makeScreenName();
    QWizardPage *makeConnection();
    QWizardPage *makePort(bool udp);
    QWizardPage *makeFolders();
    QWizardPage *makeNetworks();
    QWizardPage *makeFinish();
    void runPortTest(bool udp);
    void refreshFolders();
    void updateSummary();

    mx::Core *m_core;
    QRadioButton *m_recommended = nullptr;
    QComboBox *m_scale = nullptr;   // [ui-e] interface size
    QRadioButton *m_custom = nullptr;
    QLineEdit *m_name = nullptr;
    QList<QRadioButton *> m_conn;
    QRadioButton *m_tcpOn = nullptr, *m_tcpOff = nullptr, *m_udpOn = nullptr, *m_udpOff = nullptr;
    QLineEdit *m_tcpPort = nullptr, *m_udpPort = nullptr;
    WrapLabel *m_tcpResult = nullptr, *m_udpResult = nullptr;   // [qa-2] wraps a long answer
    QCheckBox *m_upnp = nullptr;
    PathField *m_incoming = nullptr;
    QTreeWidget *m_folderList = nullptr;
    QStringList m_folders;
    QList<QPair<QString, QCheckBox *>> m_nets;   // enabled key -> check
    QCheckBox *m_assoc = nullptr, *m_autostart = nullptr, *m_connectNow = nullptr;
    WrapLabel *m_summary = nullptr;
    bool m_applied = false;
};

} // namespace mx::ui
