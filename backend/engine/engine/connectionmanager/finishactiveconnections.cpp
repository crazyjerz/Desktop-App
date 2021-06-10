#include "finishactiveconnections.h"
#include "../openvpnversioncontroller.h"
#include "wireguardconnection.h"
#include "utils/logger.h"

#ifdef Q_OS_WIN
    #include <windows.h>
    #include "ikev2connection_win.h"
#endif

#ifdef Q_OS_MAC
    #include "restorednsmanager_mac.h"
    #include "ikev2connection_mac.h"
#endif

void FinishActiveConnections::finishAllActiveConnections(IHelper *helper)
{
#ifdef Q_OS_WIN
    finishAllActiveConnections_win(helper);
#elif defined Q_OS_MAC
    finishAllActiveConnections_mac(helper);
#elif defined Q_OS_LINUX
    //todo linux
    Q_ASSERT(false);
#endif
}

#ifdef Q_OS_WIN
void FinishActiveConnections::finishAllActiveConnections_win(IHelper *helper)
{
    finishOpenVpnActiveConnections_win(helper);
    finishIkev2ActiveConnections_win(helper);
    finishWireGuardActiveConnections_win(helper);
}

void FinishActiveConnections::finishOpenVpnActiveConnections_win(IHelper *helper)
{
    const QStringList strOpenVpnExeList = OpenVpnVersionController::instance().getAvailableOpenVpnExecutables();
    for (const QString &strExe : strOpenVpnExeList)
    {
        helper->executeTaskKill(strExe);
    }
}

void FinishActiveConnections::finishIkev2ActiveConnections_win(IHelper *helper)
{
    const QVector<HRASCONN> v = IKEv2Connection_win::getActiveWindscribeConnections();

    if (!v.isEmpty())
    {
        for (HRASCONN hRas : v)
        {
            IKEv2ConnectionDisconnectLogic_win::blockingDisconnect(hRas);
        }

        helper->disableDnsLeaksProtection();
        helper->removeHosts();
    }
}

void FinishActiveConnections::finishWireGuardActiveConnections_win(IHelper *helper)
{
    QString strWireGuardExe = WireGuardConnection::getWireGuardExeName();
    if (!strWireGuardExe.endsWith(".exe"))
        strWireGuardExe.append(".exe");
    helper->executeTaskKill(strWireGuardExe);
    helper->stopWireGuard();  // This will also reset route monitoring.
}
#elif defined Q_OS_MAC
void FinishActiveConnections::finishAllActiveConnections_mac(IHelper *helper)
{
    finishOpenVpnActiveConnections_mac(helper);
    finishWireGuardActiveConnections_mac(helper);
    IKEv2Connection_mac::closeWindscribeActiveConnection();
}

void FinishActiveConnections::finishOpenVpnActiveConnections_mac(IHelper *helper)
{
    const QStringList strOpenVpnExeList = OpenVpnVersionController::instance().getAvailableOpenVpnExecutables();
    for (const QString &strExe : strOpenVpnExeList)
    {
        helper->executeTaskKill(strExe);
    }
    helper->executeTaskKill("windscribestunnel");
    helper->executeTaskKill("windscribewstunnel");
    RestoreDNSManager_mac::restoreState(helper);
}
void FinishActiveConnections::finishWireGuardActiveConnections_mac(IHelper *helper)
{
    helper->setDefaultWireGuardDeviceName(WireGuardConnection::getWireGuardAdapterName());
    helper->stopWireGuard();
}
#endif
