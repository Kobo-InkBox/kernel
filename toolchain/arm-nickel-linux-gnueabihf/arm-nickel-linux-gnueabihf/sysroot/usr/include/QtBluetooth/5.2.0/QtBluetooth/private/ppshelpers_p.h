/***************************************************************************
**
** Copyright (C) 2012 Research In Motion
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef PPSHELPERS_H
#define PPSHELPERS_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <fcntl.h>
#include <errno.h>
#include <sys/pps.h>

#include <QSocketNotifier>
#include <QStringList>

#include <QtBluetooth/qbluetoothuuid.h>
#include <QtBluetooth/qbluetoothaddress.h>

#ifdef BT_BBPPSDEBUG
#define qBBBluetoothDebug qDebug
#else
#define qBBBluetoothDebug QT_NO_QDEBUG_MACRO
#endif

#define BT_SPP_SERVER_SUBTYPE 1
#define BT_SPP_CLIENT_SUBTYPE 2

QT_BEGIN_NAMESPACE

class BBSocketNotifier : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void distribute();
    void closeControlFD();
};

enum ResultType {UNKNOWN, EVENT, MESSAGE, RESPONSE};

struct ppsResult {
    ppsResult() : success(false), error(0) {}

    bool success;
    int id;
    QString msg;
    QStringList dat;
    QString errorMsg;
    int error;
};

QPair<int, QObject*> takeObjectInWList(int id);

void ppsRegisterControl();

void ppsUnregisterControl(QObject *obj);

pps_encoder_t *beginCtrlMessage(const char *msg, QObject *sender);

bool endCtrlMessage(pps_encoder_t *encoder);

bool ppsSendControlMessage(const char *msg, int service, const QBluetoothUuid &uuid, const QString &address, const QString &serviceName, QObject *sender=0, const int &subtype=-1);

bool ppsSendControlMessage(const char *msg,  const QString &dat, QObject *sender=0);

bool ppsSendControlMessage(const char *msg, QObject *sender=0);

void ppsDecodeControlResponse();

int openOPPControl();

void ppsSendOpp(const char *msg, const QByteArray &filename, const QBluetoothAddress &address, QObject *sender);

QVariant ppsReadSetting(const char *property);

QVariant ppsRemoteDeviceStatus(const QByteArray &address, const char *property);

bool ppsReadRemoteDevice(int fd, pps_decoder_t *decoder, QBluetoothAddress *btAddr, QString *deviceName);

void ppsRegisterForEvent(const QString &evt, QObject *obj);

void ppsUnregisterForEvent(const QString &evt, QObject *obj);

QT_END_NAMESPACE

#endif // PPSHELPERS_H
