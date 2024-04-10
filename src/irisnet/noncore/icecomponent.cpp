/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#include "icecomponent.h"

#include "iceagent.h"
#include "icelocaltransport.h"
#include "iceturntransport.h"
#include "objectsession.h"
#include "udpportreserver.h"

#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QtCrypto>
#include <stdlib.h>

template <class T> constexpr std::add_const_t<T> &as_const(T &t) noexcept { return t; }

namespace XMPP {
static int calc_priority(int typePref, int localPref, int componentId)
{
    Q_ASSERT(typePref >= 0 && typePref <= 126);
    Q_ASSERT(localPref >= 0 && localPref <= 65535);
    Q_ASSERT(componentId >= 1 && componentId <= 256);

    int priority = (1 << 24) * typePref;
    priority += (1 << 8) * localPref;
    priority += (256 - componentId);
    return priority;
}

class IceComponent::Private : public QObject {
    Q_OBJECT

public:
    class Config {
    public:
        QList<Ice176::LocalAddress> localAddrs;

        // for example manually provided external address mapped to every local
        QList<Ice176::ExternalAddress> extAddrs;

        TransportAddress stunBindAddr;

        TransportAddress stunRelayUdpAddr;
        QString          stunRelayUdpUser;
        QCA::SecureArray stunRelayUdpPass;

        TransportAddress stunRelayTcpAddr;
        QString          stunRelayTcpUser;
        QCA::SecureArray stunRelayTcpPass;
    };

    class LocalTransport {
    public:
        QUdpSocket                       *qsock;
        QHostAddress                      addr;
        QSharedPointer<IceLocalTransport> sock;
        int                               network;
        bool                              isVpn;
        bool                              started;
        bool                              stun_started;
        bool                              stun_finished, turn_finished; // candidates emitted
        QHostAddress                      extAddr;
        bool                              ext_finished;
        bool                              borrowed = false;

        LocalTransport() :
            network(-1), isVpn(false), started(false), stun_started(false), stun_finished(false), turn_finished(false),
            ext_finished(false)
        {
        }
    };

    IceComponent                      *q;
    ObjectSession                      sess;
    int                                id;
    QString                            clientSoftware;
    TurnClient::Proxy                  proxy;
    UdpPortReserver                   *portReserver = nullptr;
    Config                             pending;
    Config                             config;
    bool                               stopping = false;
    QList<LocalTransport *>            udpTransports; // transport for local host-only candidates
    QSharedPointer<IceTurnTransport>   tcpTurn;       // tcp relay candidate
    QList<Candidate>                   localCandidates;
    QHash<int, QSet<TransportAddress>> channelPeers;
    bool                               useLocal        = true; // use local host candidates
    bool                               useStunBind     = true;
    bool                               useStunRelayUdp = true;
    bool                               useStunRelayTcp = true;
    bool                               localFinished   = false;
    // bool                               stunFinished      = false;
    bool gatheringComplete = false;
    int  debugLevel        = DL_Packet;

    Private(IceComponent *_q) : QObject(_q), q(_q), sess(this) { }

    ~Private() { qDeleteAll(udpTransports); }

    LocalTransport *createLocalTransport(QUdpSocket *socket, const Ice176::LocalAddress &la)
    {
        auto lt   = new LocalTransport;
        lt->qsock = socket;
        lt->addr  = la.addr;
        lt->sock  = QSharedPointer<IceLocalTransport>::create();
        lt->sock->setDebugLevel(IceTransport::DebugLevel(debugLevel));
        lt->network = la.network;
        lt->isVpn   = la.isVpn;
        connect(lt->sock.data(), &IceLocalTransport::started, this, &Private::lt_started);
        connect(lt->sock.data(), &IceLocalTransport::stopped, this, [this, lt]() {
            if (eraseLocalTransport(lt))
                tryStopped();
        });
        connect(lt->sock.data(), &IceLocalTransport::addressesChanged, this, &Private::lt_addressesChanged);
        connect(lt->sock.data(), &IceLocalTransport::error, this, [this, lt](int error) {
            if (error == IceLocalTransport::ErrorStun) {
                lt->stun_finished = true;
                tryGatheringComplete();
            } else if (error == IceLocalTransport::ErrorTurn) {
                lt->turn_finished = true;
                tryGatheringComplete();
            } else if (eraseLocalTransport(lt))
                tryGatheringComplete();
        });
        connect(lt->sock.data(), &IceLocalTransport::debugLine, this, &Private::lt_debugLine);
        return lt;
    }

    void update(QList<QUdpSocket *> *socketList)
    {
        Q_ASSERT(!stopping);

        // only allow setting stun stuff once
        if ((pending.stunBindAddr.isValid() && !config.stunBindAddr.isValid())
            || (pending.stunRelayUdpAddr.isValid() && !config.stunRelayUdpAddr.isValid())
            || (pending.stunRelayTcpAddr.isValid() && !config.stunRelayTcpAddr.isValid())) {
            config.stunBindAddr     = pending.stunBindAddr;
            config.stunRelayUdpAddr = pending.stunRelayUdpAddr;
            config.stunRelayUdpUser = pending.stunRelayUdpUser;
            config.stunRelayUdpPass = pending.stunRelayUdpPass;
            config.stunRelayTcpAddr = pending.stunRelayTcpAddr;
            config.stunRelayTcpUser = pending.stunRelayTcpUser;
            config.stunRelayTcpPass = pending.stunRelayTcpPass;
        }

        // for now, only allow setting localAddrs once
        if (!pending.localAddrs.isEmpty() && config.localAddrs.isEmpty()) {
            for (const Ice176::LocalAddress &la : as_const(pending.localAddrs)) {
                // skip duplicate addrs
                if (findLocalAddr(la.addr) != -1)
                    continue;

                QUdpSocket *qsock = nullptr;
                if (useLocal && socketList) {
                    qsock = takeFromSocketList(socketList, la.addr, this);
                }
                bool borrowedSocket = qsock != nullptr;
                if (!qsock) {
                    // otherwise, bind to random
                    qsock = new QUdpSocket(this);
                    if (!qsock->bind(la.addr, 0)) {
                        delete qsock;
                        emit q->debugLine("Warning: unable to bind to random port.");
                        continue;
                    }
                }

                config.localAddrs += la;
                auto lt      = createLocalTransport(qsock, la);
                lt->borrowed = borrowedSocket;
                udpTransports += lt;

                if (lt->addr.protocol() != QAbstractSocket::IPv6Protocol) {
                    lt->sock->setClientSoftwareNameAndVersion(clientSoftware);
                    if (useStunBind && config.stunBindAddr.isValid()) {
                        lt->sock->setStunBindService(config.stunBindAddr);
                    }
                    if (useStunRelayUdp && config.stunRelayUdpAddr.isValid() && !config.stunRelayUdpUser.isEmpty()) {
                        lt->sock->setStunRelayService(config.stunRelayUdpAddr, config.stunRelayUdpUser,
                                                      config.stunRelayUdpPass);
                    }
                }

                int port = qsock->localPort();
                lt->sock->start(qsock);
                emit q->debugLine(QString("starting transport ") + la.addr.toString() + ';' + QString::number(port)
                                  + " for component " + QString::number(id));
            }
        }

        // extAddrs created on demand if present, but only once
        if (!pending.extAddrs.isEmpty() && config.extAddrs.isEmpty()) {
            config.extAddrs = pending.extAddrs;

            bool need_doExt = false;

            for (auto lt : as_const(udpTransports)) {
                // already assigned an ext address?  skip
                if (!lt->extAddr.isNull())
                    continue;

                auto laddr = lt->sock->localAddress();
                if (laddr.addr.protocol() == QAbstractSocket::IPv6Protocol)
                    continue;

                // find external address by address of local socket (external has to be configured that way)
                auto eaIt = std::find_if(config.extAddrs.constBegin(), config.extAddrs.constEnd(), [&](auto const &ea) {
                    return ea.base.addr == laddr.addr && (ea.portBase == -1 || ea.portBase == laddr.port);
                });

                if (eaIt != config.extAddrs.constEnd()) {
                    lt->extAddr = eaIt->addr;
                    if (lt->started)
                        need_doExt = true;
                }
            }

            if (need_doExt)
                QTimer::singleShot(0, this, [this]() {
                    if (stopping)
                        return;

                    ObjectSessionWatcher watch(&sess);

                    for (auto lt : as_const(udpTransports)) {
                        if (lt->started) {
                            int addrAt = findLocalAddr(lt->addr);
                            Q_ASSERT(addrAt != -1);

                            ensureExt(lt, addrAt); // will emit candidateAdded if everything goes well
                            if (!watch.isValid())
                                return;
                        }
                    }
                });
        }

        if (useStunRelayTcp && config.stunRelayTcpAddr.isValid() && !config.stunRelayTcpUser.isEmpty() && !tcpTurn) {
            tcpTurn = QSharedPointer<IceTurnTransport>::create();
            tcpTurn->setDebugLevel(IceTransport::DebugLevel(debugLevel));
            connect(tcpTurn.data(), &IceTurnTransport::started, this, &Private::tt_started);
            connect(tcpTurn.data(), &IceTurnTransport::stopped, this, &Private::tt_stopped);
            connect(tcpTurn.data(), &IceTurnTransport::error, this, &Private::tt_error);
            connect(tcpTurn.data(), &IceTurnTransport::debugLine, this, &Private::tt_debugLine);
            tcpTurn->setClientSoftwareNameAndVersion(clientSoftware);
            tcpTurn->setProxy(proxy);
            tcpTurn->setUsername(config.stunRelayTcpUser);
            tcpTurn->setPassword(config.stunRelayTcpPass);
            tcpTurn->start(config.stunRelayTcpAddr);

            emit q->debugLine(QLatin1String("starting TURN transport with server ") + config.stunRelayTcpAddr
                              + " for component " + QString::number(id));
        }

        if (udpTransports.isEmpty() && !localFinished) {
            localFinished = true;
            sess.defer(q, "localFinished");
        }
        sess.defer(this, "tryGatheringComplete");
    }

    void stop()
    {
        Q_ASSERT(!stopping);

        stopping = true;

        // nothing to stop?
        if (allStopped()) {
            sess.defer(this, "postStop");
            return;
        }

        for (LocalTransport *lt : as_const(udpTransports))
            lt->sock->stop();

        if (tcpTurn)
            tcpTurn->stop();
    }

    int peerReflexivePriority(QSharedPointer<IceTransport> iceTransport, int path) const
    {
        int                      addrAt = -1;
        const IceLocalTransport *lt     = qobject_cast<const IceLocalTransport *>(iceTransport.data());
        if (lt) {
            auto it = std::find_if(udpTransports.begin(), udpTransports.end(),
                                   [&](auto const &a) { return a->sock == lt; });
            Q_ASSERT(it != udpTransports.end());
            addrAt = int(std::distance(udpTransports.begin(), it));
            if (path == 1) {
                // lower priority, but not as far as IceTurnTransport
                addrAt += 512;
            }
        } else if (qobject_cast<const IceTurnTransport *>(iceTransport) == tcpTurn) {
            // lower priority by making it seem like the last nic
            addrAt = 1024;
        }

        return choose_default_priority(PeerReflexiveType, 65535 - addrAt, false, id);
    }

    void flagPathAsLowOverhead(int id, const TransportAddress &addr)
    {
        int at = -1;
        for (int n = 0; n < localCandidates.count(); ++n) {
            if (localCandidates[n].id == id) {
                at = n;
                break;
            }
        }

        Q_ASSERT(at != -1);

        if (at == -1)
            return;

        Candidate &c = localCandidates[at];

        QSet<TransportAddress> &addrs = channelPeers[c.id];
        if (!addrs.contains(addr)) {
            addrs += addr;
            c.iceTransport->addChannelPeer(addr);
        }
    }

    void addLocalPeerReflexiveCandidate(const TransportAddress &addr, IceComponent::CandidateInfo::Ptr base,
                                        quint32 priority)
    {
        auto ci  = IceComponent::CandidateInfo::Ptr::create();
        ci->addr = addr;
        ci->addr.addr.setScopeId(QString());
        ci->related     = base->addr;
        ci->base        = base->addr;
        ci->type        = IceComponent::PeerReflexiveType;
        ci->priority    = priority;
        ci->foundation  = IceAgent::instance()->foundation(IceComponent::PeerReflexiveType, ci->base.addr);
        ci->componentId = base->componentId;
        ci->network     = base->network;

        auto baseCand = std::find_if(localCandidates.begin(), localCandidates.end(), [&](auto const &c) {
            return c.info->base == base->base && c.info->type == HostType;
        });
        Q_ASSERT(baseCand != localCandidates.end());

        Candidate c;
        c.id           = getId();
        c.info         = ci;
        c.iceTransport = baseCand->iceTransport;
        c.path         = 0;

        localCandidates += c;

        emit q->candidateAdded(c);
    }

private:
    // localPref is the priority of the network interface being used for
    //   this candidate.  the value must be between 0-65535 and different
    //   interfaces must have different values.  if there is only one
    //   interface, the value should be 65535.
    static int choose_default_priority(CandidateType type, int localPref, bool isVpn, int componentId)
    {
        int typePref;
        if (type == HostType) {
            if (isVpn)
                typePref = 0;
            else
                typePref = 126;
        } else if (type == PeerReflexiveType)
            typePref = 110;
        else if (type == ServerReflexiveType)
            typePref = 100;
        else // RelayedType
            typePref = 0;

        return calc_priority(typePref, localPref, componentId);
    }

    static QUdpSocket *takeFromSocketList(QList<QUdpSocket *> *socketList, const QHostAddress &addr,
                                          QObject *parent = nullptr)
    {
        for (int n = 0; n < socketList->count(); ++n) {
            if ((*socketList)[n]->localAddress() == addr) {
                QUdpSocket *sock = socketList->takeAt(n);
                sock->setParent(parent);
                return sock;
            }
        }

        return nullptr;
    }

    int getId() const
    {
        for (int n = 0;; ++n) {
            bool found = false;
            for (const Candidate &c : localCandidates) {
                if (c.id == n) {
                    found = true;
                    break;
                }
            }

            if (!found)
                return n;
        }
    }

    int findLocalAddr(const QHostAddress &addr)
    {
        for (int n = 0; n < config.localAddrs.count(); ++n) {
            if (config.localAddrs[n].addr == addr)
                return n;
        }

        return -1;
    }

    void ensureExt(LocalTransport *lt, int addrAt)
    {
        if (!lt->extAddr.isNull() && !lt->ext_finished) {
            auto ci         = CandidateInfo::Ptr::create();
            ci->addr.addr   = lt->extAddr;
            ci->addr.port   = lt->sock->localAddress().port;
            ci->type        = ServerReflexiveType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->base        = lt->sock->localAddress();
            ci->related     = ci->base;
            ci->network     = lt->network;
            ci->foundation  = IceAgent::instance()->foundation(ServerReflexiveType, ci->base.addr);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = lt->sock;
            c.path         = 0;

            lt->ext_finished = true;

            storeLocalNotReduntantCandidate(c);
        }
    }

    void removeLocalCandidates(const QSharedPointer<IceTransport> sock)
    {
        ObjectSessionWatcher watch(&sess);

        for (int n = 0; n < localCandidates.count(); ++n) {
            Candidate &c = localCandidates[n];

            if (c.iceTransport == sock) {
                Candidate tmp = localCandidates.takeAt(n);
                --n; // adjust position

                channelPeers.remove(tmp.id);

                emit q->candidateRemoved(tmp);
                if (!watch.isValid())
                    return;
            }
        }
    }

    void storeLocalNotReduntantCandidate(const Candidate &c)
    {
        ObjectSessionWatcher watch(&sess);
        // RFC8445 5.1.3.  Eliminating Redundant Candidates
        auto it = std::find_if(localCandidates.begin(), localCandidates.end(), [&](const Candidate &cc) {
            return cc.info->addr == c.info->addr && cc.info->base == c.info->base
                && cc.info->priority >= c.info->priority;
        });
        if (it == localCandidates.end()) { // not reduntant
            localCandidates += c;
            emit q->candidateAdded(c);
        }
    }

    bool allStopped() const { return udpTransports.isEmpty() && !tcpTurn; }

    void tryStopped()
    {
        if (allStopped())
            postStop();
    }

    // return true if component is still alive after transport removal
    bool eraseLocalTransport(LocalTransport *lt)
    {
        ObjectSessionWatcher watch(&sess);

        emit q->debugLine(QLatin1String("Stopping local transport: ") + lt->sock->localAddress());
        removeLocalCandidates(lt->sock);
        if (!watch.isValid())
            return false;

        lt->sock->disconnect(this);
        if (lt->borrowed) {
            lt->qsock->disconnect(this);
            portReserver->returnSockets({ lt->qsock });
        }
        delete lt;
        udpTransports.removeOne(lt);
        return true;
    }

private slots:
    void tryGatheringComplete()
    {
        if (gatheringComplete || (tcpTurn && !tcpTurn->isStarted()))
            return;

        auto checkFinished = [&](const LocalTransport *lt) {
            return lt->started && (!lt->sock->stunBindServiceAddress().isValid() || lt->stun_finished)
                && (!lt->sock->stunRelayServiceAddress().isValid() || lt->turn_finished);
        };

        bool allFinished = true;
        for (const LocalTransport *lt : as_const(udpTransports)) {
            if (!checkFinished(lt)) {
                allFinished = false;
                break;
            }
        }

        if (allFinished) {
            gatheringComplete = true;
            emit q->gatheringComplete();
        }
    }

    void postStop()
    {
        stopping = false;

        emit q->stopped();
    }

    void lt_started()
    {
        IceLocalTransport *sock = static_cast<IceLocalTransport *>(sender());

        auto it
            = std::find_if(udpTransports.begin(), udpTransports.end(), [&](auto const &a) { return a->sock == sock; });
        Q_ASSERT(it != udpTransports.end());
        LocalTransport *lt = *it;

        lt->started = true;

        int addrAt = findLocalAddr(lt->addr);
        Q_ASSERT(addrAt != -1);

        ObjectSessionWatcher watch(&sess);

        if (useLocal) {
            auto ci         = CandidateInfo::Ptr::create();
            ci->addr        = lt->sock->localAddress();
            ci->type        = HostType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->base        = ci->addr;
            ci->network     = lt->network;
            ci->foundation  = IceAgent::instance()->foundation(HostType, ci->base.addr);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 0;

            localCandidates += c;

            emit q->candidateAdded(c);
            if (!watch.isValid())
                return;

            ensureExt(lt, addrAt);
            if (!watch.isValid())
                return;
        }

        if (!lt->stun_started) {
            lt->stun_started = true;
            if (useStunBind
                && (lt->sock->stunBindServiceAddress().isValid() || lt->sock->stunRelayServiceAddress().isValid())) {
                lt->sock->stunStart();
                if (!watch.isValid())
                    return;
            } else {
                lt->stun_finished = true;
                lt->turn_finished = true;
            }
        }

        // check completeness of various stuff
        if (!localFinished) {
            bool allStarted = true;
            for (const LocalTransport *lt : as_const(udpTransports)) {
                if (!lt->started) {
                    allStarted = false;
                    break;
                }
            }
            if (allStarted) {
                localFinished = true;
                emit q->localFinished();
                if (!watch.isValid())
                    return;
            }
        }

        tryGatheringComplete();
    }

    void lt_addressesChanged()
    {
        IceLocalTransport *sock = static_cast<IceLocalTransport *>(sender());
        auto               it
            = std::find_if(udpTransports.begin(), udpTransports.end(), [&](auto const &a) { return a->sock == sock; });

        Q_ASSERT(it != udpTransports.end());
        LocalTransport *lt = *it;

        int addrAt = findLocalAddr(lt->addr);
        Q_ASSERT(addrAt != -1);

        ObjectSessionWatcher watch(&sess);

        if (useStunBind && lt->sock->serverReflexiveAddress().isValid() && !lt->stun_finished) {
            // automatically assign ext to related leaps, if possible
            for (LocalTransport *i : as_const(udpTransports)) {
                if (i->extAddr.isNull() && i->sock->localAddress() == lt->sock->localAddress()) {
                    i->extAddr = lt->sock->serverReflexiveAddress().addr;
                    if (i->started) {
                        ensureExt(i, addrAt);
                        if (!watch.isValid())
                            return;
                    }
                }
            }

            auto ci         = CandidateInfo::Ptr::create();
            ci->addr        = lt->sock->serverReflexiveAddress();
            ci->base        = lt->sock->localAddress();
            ci->related     = ci->base;
            ci->type        = ServerReflexiveType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->network     = lt->network;
            ci->foundation  = IceAgent::instance()->foundation(
                ServerReflexiveType, ci->base.addr, lt->sock->reflexiveAddressSource(), QAbstractSocket::UdpSocket);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 0;

            lt->stun_finished = true;

            storeLocalNotReduntantCandidate(c);
        } else if (useStunBind && !lt->sock->isStunAlive() && !lt->stun_finished) {
            lt->stun_finished = true;
        }

        if (lt->sock->relayedAddress().isValid() && !lt->turn_finished) {
            auto ci         = CandidateInfo::Ptr::create();
            ci->addr        = lt->sock->relayedAddress();
            ci->base        = ci->addr;
            ci->related     = lt->sock->serverReflexiveAddress();
            ci->type        = RelayedType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->network     = lt->network;
            ci->foundation  = IceAgent::instance()->foundation(
                RelayedType, ci->base.addr, lt->sock->stunRelayServiceAddress().addr, QAbstractSocket::UdpSocket);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 1;

            lt->turn_finished = true;

            storeLocalNotReduntantCandidate(c);
        } else if (!lt->sock->isTurnAlive() && !lt->turn_finished) {
            lt->turn_finished = true;
        }
        if (!watch.isValid())
            return;

        tryGatheringComplete();
    }

    void lt_debugLine(const QString &line) { emit q->debugLine(line); }

    void tt_started()
    {
        // lower priority by making it seem like the last nic
        int addrAt = 1024;

        auto ci         = CandidateInfo::Ptr::create();
        ci->addr        = tcpTurn->relayedAddress();
        ci->related     = tcpTurn->reflexiveAddress();
        ci->type        = RelayedType;
        ci->componentId = id;
        ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, false, ci->componentId);
        ci->base        = ci->addr;
        ci->network     = 0; // not relevant
        ci->foundation  = IceAgent::instance()->foundation(RelayedType, ci->base.addr, config.stunRelayTcpAddr.addr,
                                                           QAbstractSocket::TcpSocket);

        Candidate c;
        c.id           = getId();
        c.info         = ci;
        c.iceTransport = tcpTurn->sharedFromThis();
        c.path         = 0;

        localCandidates += c;

        emit q->candidateAdded(c);

        tryGatheringComplete();
    }

    void tt_stopped()
    {
        ObjectSessionWatcher watch(&sess);

        removeLocalCandidates(tcpTurn->sharedFromThis());
        if (!watch.isValid())
            return;

        tcpTurn->disconnect(this);
        tcpTurn.reset();

        tryStopped();
    }

    void tt_error(int e)
    {
        Q_UNUSED(e)

        ObjectSessionWatcher watch(&sess);

        removeLocalCandidates(tcpTurn);
        if (!watch.isValid())
            return;

        tcpTurn->disconnect(this);
        tcpTurn.reset();
        tryGatheringComplete();
    }

    void tt_debugLine(const QString &line) { emit q->debugLine(line); }
};

IceComponent::IceComponent(int id, QObject *parent) : QObject(parent)
{
    d     = new Private(this);
    d->id = id;
}

IceComponent::~IceComponent() { delete d; }

int IceComponent::id() const { return d->id; }

bool IceComponent::isGatheringComplete() const { return d->gatheringComplete; }

void IceComponent::setClientSoftwareNameAndVersion(const QString &str) { d->clientSoftware = str; }

void IceComponent::setProxy(const TurnClient::Proxy &proxy) { d->proxy = proxy; }

void IceComponent::setPortReserver(UdpPortReserver *portReserver) { d->portReserver = portReserver; }

UdpPortReserver *IceComponent::portReserver() const { return d->portReserver; }

void IceComponent::setLocalAddresses(const QList<Ice176::LocalAddress> &addrs) { d->pending.localAddrs = addrs; }

void IceComponent::setExternalAddresses(const QList<Ice176::ExternalAddress> &addrs) { d->pending.extAddrs = addrs; }

void IceComponent::setStunBindService(const TransportAddress &addr) { d->pending.stunBindAddr = addr; }

void IceComponent::setStunRelayUdpService(const TransportAddress &addr, const QString &user,
                                          const QCA::SecureArray &pass)
{
    d->pending.stunRelayUdpAddr = addr;
    d->pending.stunRelayUdpUser = user;
    d->pending.stunRelayUdpPass = pass;
}

void IceComponent::setStunRelayTcpService(const TransportAddress &addr, const QString &user,
                                          const QCA::SecureArray &pass)
{
    d->pending.stunRelayTcpAddr = addr;
    d->pending.stunRelayTcpUser = user;
    d->pending.stunRelayTcpPass = pass;
}

void IceComponent::setUseLocal(bool enabled) { d->useLocal = enabled; }

void IceComponent::setUseStunBind(bool enabled) { d->useStunBind = enabled; }

void IceComponent::setUseStunRelayUdp(bool enabled) { d->useStunRelayUdp = enabled; }

void IceComponent::setUseStunRelayTcp(bool enabled) { d->useStunRelayTcp = enabled; }

void IceComponent::update(QList<QUdpSocket *> *socketList) { d->update(socketList); }

void IceComponent::stop() { d->stop(); }

int IceComponent::peerReflexivePriority(QSharedPointer<IceTransport> iceTransport, int path) const
{
    return d->peerReflexivePriority(iceTransport, path);
}

void IceComponent::addLocalPeerReflexiveCandidate(const TransportAddress &addr, IceComponent::CandidateInfo::Ptr base,
                                                  quint32 priority)
{
    d->addLocalPeerReflexiveCandidate(addr, base, priority);
}

void IceComponent::flagPathAsLowOverhead(int id, const TransportAddress &addr)
{
    return d->flagPathAsLowOverhead(id, addr);
}

void IceComponent::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    for (const Private::LocalTransport *lt : as_const(d->udpTransports))
        lt->sock->setDebugLevel(IceTransport::DebugLevel(level));
    if (d->tcpTurn)
        d->tcpTurn->setDebugLevel((IceTransport::DebugLevel)level);
}

IceComponent::CandidateInfo::Ptr
IceComponent::CandidateInfo::makeRemotePrflx(int componentId, const TransportAddress &fromAddr, quint32 priority)
{
    auto c  = IceComponent::CandidateInfo::Ptr::create();
    c->addr = fromAddr;
    c->addr.addr.setScopeId(QString());
    c->type        = PeerReflexiveType;
    c->priority    = priority;
    c->foundation  = QUuid::createUuid().toString();
    c->componentId = componentId;
    c->network     = -1;
    return c;
}

} // namespace XMPP

#include "icecomponent.moc"
