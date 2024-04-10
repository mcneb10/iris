/*
 * Copyright (C) 2009-2010  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef ICE176_H
#define ICE176_H

#include "turnclient.h"

#include <QHostAddress>
#include <QObject>
#include <QString>

namespace QCA {
class SecureArray;
}

namespace XMPP {
class UdpPortReserver;
class AbstractStunDisco;

class Ice176 : public QObject {
    Q_OBJECT

public:
    enum Error { ErrorGeneric, ErrorDisconnected };

    enum Mode { Initiator, Responder };

    class LocalAddress {
    public:
        QHostAddress addr;
        int          network = -1; // -1 = unknown
        bool         isVpn   = false;
    };

    class ExternalAddress {
    public:
        LocalAddress base;
        QHostAddress addr;
        int          portBase; // -1 = same as base

        ExternalAddress() : portBase(-1) { }
    };

    class Candidate {
    public:
        int          component = -1;
        QString      foundation;
        int          generation = -1;
        QString      id;
        QHostAddress ip;
        int          network  = -1; // -1 = unknown
        int          port     = -1;
        int          priority = -1;
        QString      protocol;
        QHostAddress rel_addr;
        int          rel_port = -1;
        QHostAddress rem_addr;
        int          rem_port = -1;
        QString      type;
    };

    struct SelectedCandidate {
        QHostAddress ip;
        int          port        = -1;
        int          componentId = -1;
    };

    Ice176(QObject *parent = nullptr);
    ~Ice176();

    void reset();

    void setProxy(const TurnClient::Proxy &proxy);

    // if set, ports will be drawn from the reserver if possible, before
    //   binding to random ports
    // note: ownership is not passed
    void setPortReserver(UdpPortReserver *portReserver);

    void setLocalAddresses(const QList<LocalAddress> &addrs);

    // one per local address.  you must set local addresses first.
    void setExternalAddresses(const QList<ExternalAddress> &addrs);

    void setStunBindService(const QHostAddress &addr, quint16 port); // REVIEW if we need both v4 and v6?
    void setStunRelayUdpService(const QHostAddress &addr, quint16 port, const QString &user,
                                const QCA::SecureArray &pass);
    void setStunRelayTcpService(const QHostAddress &addr, quint16 port, const QString &user,
                                const QCA::SecureArray &pass);

    // these all start out enabled, but can be disabled for diagnostic
    //   purposes
    void setUseLocal(bool enabled);
    void setUseStunBind(bool enabled);
    void setUseStunRelayUdp(bool enabled);
    void setUseStunRelayTcp(bool enabled);
    void setAllowIpExposure(bool enabled);
    void setStunDiscoverer(AbstractStunDisco *discoverer);

    void setComponentCount(int count);

    enum Feature {
        Trickle              = 0x1, // additional candidates will be sent later when discovered
        AggressiveNomination = 0x2, // all the candidates are nominated. so select by priority
        NotNominatedData     = 0x4, // Data on valid but not nominated candidates is allowed
        RTPOptimization      = 0x8, // Different formula for RTO, not used in RFC8445
        GatheringComplete    = 0x10 // Looks MUST in XEP-0371 but missed in XEP-0176
    };
    Q_DECLARE_FLAGS(Features, Feature)

    void setLocalFeatures(const Features &features);
    void setRemoteFeatures(const Features &features);

    void start(Mode mode); // init everything and prepare candidates
    void stop();
    bool isStopped() const;
    void startChecks(); // actually start doing checks when connection is accepted

    QString localUfrag() const;
    QString localPassword() const;

    void setRemoteCredentials(const QString &ufrag, const QString &pass);
    void addRemoteCandidates(const QList<Candidate> &list);
    void setRemoteGatheringComplete();
    void setRemoteSelectedCandidadates(const QList<SelectedCandidate> &list);

    bool       canSendMedia() const;
    bool       hasPendingDatagrams(int componentIndex) const;
    QByteArray readDatagram(int componentIndex);
    void       writeDatagram(int componentIndex, const QByteArray &datagram);

    // this call will ensure that TURN headers are minimized on this
    //   component, with the drawback that packets might not be able to
    //   be set as non-fragmentable.  use this on components that expect
    //   to send lots of very small packets, where header overhead is the
    //   most costly but also where fragmentation is impossible anyway.
    //   in short, use this on audio, but not on video.
    void flagComponentAsLowOverhead(int componentIndex);

    // FIXME: this should probably be in netinterface.h or such
    static bool isIPv6LinkLocalAddress(const QHostAddress &addr);

    void changeThread(QThread *thread);

    bool isLocalGatheringComplete() const;
    bool isActive() const;

    QList<SelectedCandidate> selectedCandidates() const;

    static QList<QHostAddress> availableNetworkAddresses();

signals:
    // indicates that the ice engine is started and is ready to receive
    //   peer creds and remote candidates
    void started();

    void stopped();
    void error(XMPP::Ice176::Error e);

    void localCandidatesReady(const QList<XMPP::Ice176::Candidate> &list);
    void localGatheringComplete();
    void readyToSendMedia();        // Has at least one valid candidate for each component
    void componentReady(int index); // has valid nominated candidate for component with index
    void iceFinished();             // Final nominated candidates are selected for all components

    void readyRead(int componentIndex);
    void datagramsWritten(int componentIndex, int count);

private:
    class Private;
    friend class Private;
    Private *d;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(Ice176::Features)
} // namespace XMPP

#endif // ICE176_H
