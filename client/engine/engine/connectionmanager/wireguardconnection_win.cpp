#include <QScopeGuard>
#include <QStandardPaths>
#include <QTimer>

#include "wireguardconnection_win.h"
#include "wireguardringlogger.h"

#include "adapterutils_win.h"
#include "engine/wireguardconfig/wireguardconfig.h"
#include "types/wireguardtypes.h"
#include "utils/crashhandler.h"
#include "utils/logger.h"
#include "utils/winutils.h"
#include "utils/ws_assert.h"

// Useful code:
// - mozilla-vpn-client\src\platforms\windows\daemon\wireguardutilswindows.cpp line 106 has code
//   for getting the interface LUID from the service name, rather than us having to hunt through
//   the registry.

// Design Notes:
// - IConnection::interfaceUpdated signal is not currently used in Engine::onConnectionManagerInterfaceUpdated
//   on Windows, so no need to emit it.

static const QString serviceIdentifier("WindscribeWireguard");

WireGuardConnection::WireGuardConnection(QObject *parent, IHelper *helper)
    : IConnection(parent),
      helper_(dynamic_cast<Helper_win*>(helper)),
      stopRequested_(false)
{
}

WireGuardConnection::~WireGuardConnection()
{
    if (isRunning()) {
        stopRequested_ = true;
        quit();
        wait();
    }
}

void WireGuardConnection::startConnect(const QString &configPathOrUrl, const QString &ip,
                                       const QString &dnsHostName, const QString &username,
                                       const QString &password, const types::ProxySettings &proxySettings,
                                       const WireGuardConfig *wireGuardConfig,
                                       bool isEnableIkev2Compression, bool isAutomaticConnectionMode)
{
    Q_UNUSED(configPathOrUrl);
    Q_UNUSED(ip);
    Q_UNUSED(dnsHostName);
    Q_UNUSED(username);
    Q_UNUSED(password);
    Q_UNUSED(proxySettings);
    Q_UNUSED(isEnableIkev2Compression);

    WS_ASSERT(helper_ != nullptr);
    WS_ASSERT(wireGuardConfig != nullptr);

    if (isRunning()) {
        stopRequested_ = true;
        quit();
        wait();
    }

    connectedSignalEmited_ = false;
    isAutomaticConnectionMode_ = isAutomaticConnectionMode;
    stopRequested_ = false;
    wireGuardConfig_ = wireGuardConfig;
    serviceCtrlManager_.unblockStartStopRequests();

    start(LowPriority);
}

void WireGuardConnection::startDisconnect()
{
    if (isRunning()) {
        stopRequested_ = true;
        serviceCtrlManager_.blockStartStopRequests();
        quit();
    }
    else if (isDisconnected()) {
        emit disconnected();
    }
}

bool WireGuardConnection::isDisconnected() const
{
    DWORD dwStatus = SERVICE_STOPPED;
    try {
        if (serviceCtrlManager_.isServiceOpen()) {
            dwStatus = serviceCtrlManager_.queryServiceStatus();
        }
    }
    catch (std::system_error& ex) {
        qCDebug(LOG_CONNECTION) << "WireGuardConnection::isDisconnected -" << ex.what();
    }

    return (dwStatus == SERVICE_STOPPED || dwStatus == SERVICE_STOP_PENDING);
}

void WireGuardConnection::run()
{
    BIND_CRASH_HANDLER_FOR_THREAD();

    qCDebug(LOG_CONNECTION) << "Starting" << getWireGuardExeName();

    // Design Notes:
    // The wireguard embedded DLL service requires that the name of the configuration file we
    // create matches the name of the service the helper installs.  The helper will install
    // the service using the name WireGuardTunnel$ConfFileName

    QString configFile = tr("%1/%2.conf").arg(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation), serviceIdentifier);

    // Installing the wireguard service requires admin privilege.
    IHelper::ExecuteError err = helper_->startWireGuard(getWireGuardExeName(), configFile);
    if (err != IHelper::EXECUTE_SUCCESS)
    {
        qCDebug(LOG_CONNECTION) << "Windscribe service could not install the WireGuard service";
        emit error((err == IHelper::EXECUTE_VERIFY_ERROR ? CONNECT_ERROR::EXE_VERIFY_WIREGUARD_ERROR
                                                         : CONNECT_ERROR::WIREGUARD_CONNECTION_ERROR));
        emit disconnected();
        return;
    }

    auto stopWireGuard = qScopeGuard([&]
    {
        serviceCtrlManager_.closeSCM();
        helper_->stopWireGuard();
    });

    // If there was a running instance of the wireguard service, the helper (startWireGuard call) will
    // have stopped it and it will have deleted the existing config file.  Therefore, don't create our
    // new config file until we're sure the wireguard service is stopped.
    if (!wireGuardConfig_->generateConfigFile(configFile)) {
        emit error(CONNECT_ERROR::WIREGUARD_CONNECTION_ERROR);
        emit disconnected();
        return;
    }

    // The wireguard service creates the log file in the same folder as the config file we passed to it.
    // We must create this log file watcher before we start the wireguard service to ensure we get
    // all log entries.
    QString logFile = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + tr("/log.bin");
    wireguardLog_.reset(new wsl::WireguardRingLogger(logFile));

    bool disableDNSLeakProtection = false;
    bool serviceStarted = startService();

    if (serviceStarted) {
        helper_->enableDnsLeaksProtection();
        disableDNSLeakProtection = true;

        // If the wireguard service indicates that it has started, the adapter and tunnel are up.
        // Let's check if the client-server handshake, which indicates the tunnel is good-to-go, has happened yet.
        // onGetWireguardLogUpdates() is much less 'expensive' than calling onGetWireguardStats().
        onGetWireguardLogUpdates();
        if (!connectedSignalEmited_) {
            onGetWireguardStats();
        }

        QScopedPointer< QTimer > timerGetWireguardStats(new QTimer);
        connect(timerGetWireguardStats.data(), &QTimer::timeout, this, &WireGuardConnection::onGetWireguardStats);
        timerGetWireguardStats->start(5000);

        QScopedPointer< QTimer > timerCheckServiceRunning(new QTimer);
        connect(timerCheckServiceRunning.data(), &QTimer::timeout, this, &WireGuardConnection::onCheckServiceRunning);
        timerCheckServiceRunning->start(2000);

        QScopedPointer< QTimer > timerGetWireguardLogUpdates(new QTimer);
        connect(timerGetWireguardLogUpdates.data(), &QTimer::timeout, this, &WireGuardConnection::onGetWireguardLogUpdates);
        timerGetWireguardLogUpdates->start(250);

        QScopedPointer< QTimer > timerTimeoutForAutomatic;
        if (isAutomaticConnectionMode_) {
            timerTimeoutForAutomatic.reset(new QTimer);
            timerTimeoutForAutomatic->setSingleShot(true);
            connect(timerTimeoutForAutomatic.data(), &QTimer::timeout, this, &WireGuardConnection::onAutomaticConnectionTimeout);
            timerTimeoutForAutomatic->start(kTimeoutForAutomatic);
        }

        if (!stopRequested_) {
            exec();
        }

        if (!timerTimeoutForAutomatic.isNull()) {
            disconnect(timerTimeoutForAutomatic.data(), &QTimer::timeout, nullptr, nullptr);
            timerTimeoutForAutomatic->stop();
        }

        disconnect(timerGetWireguardStats.data(), &QTimer::timeout, nullptr, nullptr);
        disconnect(timerCheckServiceRunning.data(), &QTimer::timeout, nullptr, nullptr);
        disconnect(timerGetWireguardLogUpdates.data(), &QTimer::timeout, nullptr, nullptr);

        timerGetWireguardStats->stop();
        timerCheckServiceRunning->stop();
        timerGetWireguardLogUpdates->stop();

        // Get final receive/transmit byte counts.
        onGetWireguardStats();
    }

    serviceCtrlManager_.closeSCM();

    if (helper_->stopWireGuard()) {
        configFile.clear();
    }
    else {
        qCDebug(LOG_CONNECTION) << "WireGuardConnection::run - windscribe service failed to stop the WireGuard service instance";
    }

    stopWireGuard.dismiss();

    wireguardLog_->getFinalLogEntries();

    // Ensure the config file is deleted if something went awry during service startup.  If all goes well,
    // the wireguard service will delete the file when it exits.
    if (!configFile.isEmpty() && QFile::exists(configFile)) {
        QFile::remove(configFile);
    }

    // Delay emiting signals until we have cleaned up all our resources.
    if (wireguardLog_->adapterSetupFailed()) {
        emit error(CONNECT_ERROR::WIREGUARD_ADAPTER_SETUP_FAILED);
    }
    else if (!serviceStarted) {
        emit error(CONNECT_ERROR::WIREGUARD_CONNECTION_ERROR);
    }

    emit disconnected();

    qCDebug(LOG_CONNECTION) << "WireGuardConnection::run exiting";

    if (disableDNSLeakProtection) {
        helper_->disableDnsLeaksProtection();
    }

    wireguardLog_.reset();
}

void WireGuardConnection::onCheckServiceRunning()
{
    if (isDisconnected()) {
        qCDebug(LOG_CONNECTION) << "The WireGuard service has stopped unexpectedly";
        quit();
    }
}

void WireGuardConnection::onGetWireguardLogUpdates()
{
    if (!wireguardLog_.isNull()) {
        wireguardLog_->getNewLogEntries();

        if (!connectedSignalEmited_ && wireguardLog_->isTunnelRunning()) {
            onTunnelConnected();
        }

        // We must rely on the WireGuard service log to detect handshake failures.  The service itself does
        // not provide a mechanism for detecting such a failure.
        if (wireguardLog_->isTunnelRunning() && wireguardLog_->handshakeFailed()) {
            onWireguardHandshakeFailure();
        }
    }
}

void WireGuardConnection::onGetWireguardStats()
{
    // We have to ask the helper to do this for us, as this process lacks permission to
    // access the API provided by the wireguard-nt kernel driver instance created by the
    // wireguard service.

    types::WireGuardStatus status;
    if (helper_->getWireGuardStatus(&status)) {
        if (status.state == types::WireGuardState::ACTIVE)
        {
            if (!connectedSignalEmited_ && status.lastHandshake > 0) {
                onTunnelConnected();
            }

            emit statisticsUpdated(status.bytesReceived, status.bytesTransmitted, true);
        }
    }
}

void WireGuardConnection::onAutomaticConnectionTimeout()
{
    if (!connectedSignalEmited_) {
        emit error(CONNECT_ERROR::STATE_TIMEOUT_FOR_AUTOMATIC);
        quit();
    }
}

QString WireGuardConnection::getWireGuardExeName()
{
    return QString("WireguardService");
}

QString WireGuardConnection::getWireGuardAdapterName()
{
    return QString("WireGuardTunnel");
}

void WireGuardConnection::onWireguardHandshakeFailure()
{
    auto haveInternet = WinUtils::haveInternetConnectivity();
    if (!haveInternet.has_value()) {
        qCDebug(LOG_CONNECTION) << "The WireGuard service reported a handshake failure, but the Internet connectivity check failed.";
        return;
    }

    if (*haveInternet) {
        types::WireGuardStatus status;
        if (helper_->getWireGuardStatus(&status) && (status.state == types::WireGuardState::ACTIVE) && (status.lastHandshake > 0)) {
            // The handshake should occur every ~2 minutes.  After 3 minutes, the server will discard our key
            // information and will silently reject anything we send to it until we make another wgconfig API call.
            QDateTime lastHandshake = QDateTime::fromSecsSinceEpoch((status.lastHandshake / 10000000) - 11644473600LL, Qt::UTC);
            qint64 secsTo = lastHandshake.secsTo(QDateTime::currentDateTimeUtc());

            if (secsTo >= 3*60) {
                qCDebug(LOG_CONNECTION) << secsTo << "seconds have passed since the last WireGuard handshake, disconnecting the tunnel.";
                quit();
            }
        }
    }
    else {
        qCDebug(LOG_CONNECTION) << "The WireGuard service reported a handshake failure and Windows reports no Internet connectivity, disconnecting the tunnel.";
        quit();
    }
}

bool WireGuardConnection::startService()
{
    try {
        QString serviceName = tr("WireGuardTunnel$%1").arg(serviceIdentifier);
        serviceCtrlManager_.openSCM(SC_MANAGER_CONNECT);
        serviceCtrlManager_.openService(qPrintable(serviceName), SERVICE_QUERY_STATUS | SERVICE_START);
        serviceCtrlManager_.startService();
        return true;
    }
    catch (std::system_error& ex) {
        qCDebug(LOG_CONNECTION) << ex.what();
    }

    return false;
}

void WireGuardConnection::onTunnelConnected()
{
    connectedSignalEmited_ = true;
    AdapterGatewayInfo info = AdapterUtils_win::getWireguardConnectedAdapterInfo(serviceIdentifier);
    emit connected(info);
}
