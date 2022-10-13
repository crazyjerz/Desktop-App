#include "autoconnsettingspolicy.h"

#include <QDataStream>
#include <QSettings>
#include "utils/ws_assert.h"
#include "utils/logger.h"

int AutoConnSettingsPolicy::failedIkev2Counter_ = 0;

AutoConnSettingsPolicy::AutoConnSettingsPolicy(QSharedPointer<locationsmodel::BaseLocationInfo> bli,
                                               const types::PortMap &portMap, bool isProxyEnabled)
{
    attemps_.clear();
    curAttempt_ = 0;
    bIsAllFailed_ = false;
    isFailedIkev2CounterAlreadyIncremented_ = false;
    portMap_ = portMap;
    locationInfo_ = qSharedPointerDynamicCast<locationsmodel::MutableLocationInfo>(bli);
    WS_ASSERT(!locationInfo_.isNull());
    WS_ASSERT(!locationInfo_->locationId().isCustomConfigsLocation());

    // remove wstunnel and WireGuard protocols from portMap_ for automatic connection mode
    /*QVector<types::PortItem>::iterator it = portMap_.items().begin();
    while (it != portMap_.items().end())
    {
        if (it->protocol == PROTOCOL::WSTUNNEL ||
            it->protocol == PROTOCOL::WIREGUARD)
        {
            it = portMap_.items().erase(it);
        }
        else
        {
            ++it;
        }
    }*/

    PROTOCOL lastSuccessProtocolSaved;
    QSettings settings;
    if (settings.contains("successConnectionProtocol"))
    {
        QByteArray arr = settings.value("successConnectionProtocol").toByteArray();
        QDataStream stream(&arr, QIODevice::ReadOnly);
        QString strProtocol;
        stream >> strProtocol;
        lastSuccessProtocolSaved = PROTOCOL::fromString(strProtocol);
    }


    QVector<AttemptInfo> localAttemps;
    for (int portMapInd = 0; portMapInd < portMap_.items().count(); ++portMapInd)
    {
        // skip udp protocol, if proxy enabled
        if (isProxyEnabled && portMap_.items()[portMapInd].protocol == PROTOCOL::OPENVPN_UDP)
        {
            continue;
        }
        // skip ikev2 protocol if failed ikev2 attempts >= MAX_IKEV2_FAILED_ATTEMPTS
        if (failedIkev2Counter_ >= MAX_IKEV2_FAILED_ATTEMPTS && portMap_.items()[portMapInd].protocol.isIkev2Protocol())
        {
            continue;
        }

        AttemptInfo attemptInfo;
        attemptInfo.protocol = portMap_.items()[portMapInd].protocol;
        WS_ASSERT(portMap_.items()[portMapInd].ports.count() > 0);
        attemptInfo.portMapInd = portMapInd;
        attemptInfo.changeNode = false;

        localAttemps << attemptInfo;
    }

    if (localAttemps.count() > 0)
    {
        localAttemps.last().changeNode = true;
    }

    // if we have successfully saved connection settings, then use it first (move on top list)
    // but if first protocol ikev2, then use it second
    if (lastSuccessProtocolSaved != PROTOCOL::UNINITIALIZED)
    {
        AttemptInfo firstAttemptInfo;
        bool bFound = false;
        for (int i = 0; i < localAttemps.count(); ++i)
        {
            if (localAttemps[i].protocol == lastSuccessProtocolSaved)
            {
                firstAttemptInfo = localAttemps[i];
                localAttemps.remove(i);
                bFound = true;
                break;
            }
        }
        if (bFound)
        {
            if (localAttemps.count() > 0 && localAttemps.first().protocol.isIkev2Protocol())
            {
                localAttemps.insert(1, firstAttemptInfo);
            }
            else
            {
                localAttemps.insert(0, firstAttemptInfo);
            }
        }
    }

    // copy sorted localAttemps to attemps_
    for (int nodeInd = 0; nodeInd < locationInfo_->nodesCount(); ++nodeInd)
    {
        attemps_ << localAttemps;
    }

    // duplicate attempts (because we need do all attempts twice)
    attemps_ << attemps_;
}

void AutoConnSettingsPolicy::reset()
{
    curAttempt_ = 0;
    bIsAllFailed_ = false;
}

void AutoConnSettingsPolicy::debugLocationInfoToLog() const
{
    qCDebug(LOG_CONNECTION) << "Connection settings: automatic";
    qCDebug(LOG_CONNECTION) << locationInfo_->getLogString();
}

void AutoConnSettingsPolicy::putFailedConnection()
{
    if (!bStarted_)
    {
        return;
    }

    if (attemps_[curAttempt_].protocol.isIkev2Protocol() && !isFailedIkev2CounterAlreadyIncremented_)
    {
        failedIkev2Counter_++;
        isFailedIkev2CounterAlreadyIncremented_ = true;
    }

    if (curAttempt_ < (attemps_.count() - 1))
    {
        if (attemps_[curAttempt_].changeNode)
        {
            locationInfo_->selectNextNode();
        }
        curAttempt_++;
    }
    else
    {
        bIsAllFailed_ = true;
    }
}

bool AutoConnSettingsPolicy::isFailed() const
{
    if (!bStarted_)
    {
        return false;
    }
    return bIsAllFailed_;
}

CurrentConnectionDescr AutoConnSettingsPolicy::getCurrentConnectionSettings() const
{
    CurrentConnectionDescr ccd;

    ccd.connectionNodeType = CONNECTION_NODE_DEFAULT;
    ccd.protocol = attemps_[curAttempt_].protocol;
    ccd.port = portMap_.const_items()[attemps_[curAttempt_].portMapInd].ports[0];

    int useIpInd = portMap_.getUseIpInd(ccd.protocol);
    ccd.ip = locationInfo_->getIpForSelectedNode(useIpInd);
    ccd.hostname = locationInfo_->getHostnameForSelectedNode();
    ccd.dnsHostName = locationInfo_->getDnsName();
    ccd.wgPeerPublicKey = locationInfo_->getWgPubKeyForSelectedNode();
    ccd.verifyX509name = locationInfo_->getVerifyX509name();

    // for static IP set additional fields
    if (locationInfo_->locationId().isStaticIpsLocation())
    {
        ccd.connectionNodeType = CONNECTION_NODE_STATIC_IPS;
        ccd.username = locationInfo_->getStaticIpUsername();
        ccd.password = locationInfo_->getStaticIpPassword();
        ccd.staticIpPorts = locationInfo_->getStaticIpPorts();

        // for static ip with wireguard protocol override id to wg_ip
        if (ccd.protocol == PROTOCOL::WIREGUARD )
        {
            ccd.ip = locationInfo_->getWgIpForSelectedNode();
        }
    }

    return ccd;
}

void AutoConnSettingsPolicy::saveCurrentSuccessfullConnectionSettings()
{
    // reset ikev2 failed attempts counter if we connected with ikev2 successfully
    if (attemps_[curAttempt_].protocol.isIkev2Protocol())
    {
        failedIkev2Counter_ = 0;
    }

    QSettings settings;
    QString protocol = attemps_[curAttempt_].protocol.toLongString();
    qCDebug(LOG_CONNECTION) << "Save latest successfully connection protocol:" << protocol;

    QByteArray arr;
    {
        QDataStream stream(&arr, QIODevice::WriteOnly);
        stream << protocol;
    }
    settings.setValue("successConnectionProtocol", arr);
}

bool AutoConnSettingsPolicy::isAutomaticMode()
{
    return true;
}

void AutoConnSettingsPolicy::resolveHostnames()
{
    // nothing todo
    emit hostnamesResolved();
}
