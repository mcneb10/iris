/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
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

#ifndef STUNBINDING_H
#define STUNBINDING_H

#include <QObject>

namespace XMPP {
class StunTransactionPool;
class TransportAddress;

class StunBinding : public QObject {
    Q_OBJECT

public:
    enum Error { ErrorGeneric, ErrorTimeout, ErrorRejected, ErrorProtocol, ErrorConflict };

    StunBinding(StunTransactionPool *pool);
    ~StunBinding();

    // for ICE-use only
    void    setPriority(quint32 i);
    quint32 priority() const;
    void    setUseCandidate(bool enabled);
    bool    useCandidate() const;
    void    setIceControlling(quint64 i);
    void    setIceControlled(quint64 i);

    void setShortTermUsername(const QString &username);
    void setShortTermPassword(const QString &password);

    void setFingerprintRequired(bool enabled);

    void start();
    void start(const XMPP::TransportAddress &addr); // use addr association
    void cancel();

    const TransportAddress &reflexiveAddress() const;

    // non-translatable diagnostic string for convenience
    QString errorString() const;

signals:
    void success();
    void error(XMPP::StunBinding::Error e);

private:
    Q_DISABLE_COPY(StunBinding)

    class Private;
    friend class Private;
    Private *d;
};
} // namespace XMPP

#endif // STUNBINDING_H
