#include "wpap2p.h"

#include <QDebug>
#include <QDir>
#include <QTimer>
#include <signal.h>
#include <sys/types.h>

#define CREATE_GROUP "p2p_group_add\n"
#define GET_PEERS "p2p_peers\n"
#define GET_STATUS "status\n"
#define P2P_FIND "p2p_find %1\n"
#define REMOVE_GROUP "p2p_group_remove %1\n"
#define SET_COMMAND "set %1 %2\n"
#define WPA_PROCESS_NAME "wpa_supplicant"
#define TIMEOUT 20000           // 20s


Q_PID proc_find(const QString &name)
{
    bool ok;
    QDir dir;

    dir = QDir("/proc");
    if (!dir.exists()) {
        qCritical() << "can't open /proc";
        return -1;
    }

    foreach(QString fileName, dir.entryList()) {
        long lpid = fileName.toLong(&ok, 10);
        if (!ok)
            continue;

        QFile file(QString("/proc/%1/cmdline").arg(lpid));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QByteArray cmd = file.readLine();
            if (cmd.contains(name.toAscii())) {
                file.close();
                return (Q_PID)lpid;
            }
            file.close();
        }
    }

    return -1;
}

WPAp2p::WPAp2p(QObject *parent)
    :QThread(parent),
     WPAProcess(0)
{
    hasGroup = false;
    currentAction = NONE;
    connect(&WPAProcess, SIGNAL(readyReadStandardOutput()),
            this, SLOT(readWPAStandartOutput()));

    WPAPid = proc_find(WPA_PROCESS_NAME);
}

WPAp2p::~WPAp2p()
{
    WPAProcess.close();
    WPAProcess.kill();
}

void WPAp2p::getPeers()
{
    if (WPAPid == -1) return;

    ActionValue action = {SCAN_RESULT, 0};
    actionsQueue.enqueue(action);
}

void WPAp2p::readWPAStandartOutput()
{
    if (currentAction == NONE)
        return;

    ActionValue actionStatus = {GETTING_STATUS, 0};
    int index;
    QString value(WPAProcess.read(WPAProcess.bytesAvailable()));
    QStringList devices;

    mutex.lock();
    switch (currentAction) {
    case GETTING_STATUS:
        if ((index = value.indexOf("wpa_state=")) > -1) {
            emit status(value.mid(index + 10, value.indexOf("\n", index)
                                  - index - 10));
        }
        break;
    case START_GROUP:
        if (value.contains("OK")) {
            emit groupStarted();
            hasGroup = true;
            actionsQueue.enqueue(actionStatus);
        }
        break;
    case STOP_GROUP:
        if (value.contains("OK")) {
            emit groupStopped();
            hasGroup = false;
            actionsQueue.enqueue(actionStatus);
        }
        break;
    case SCANNING:
        if (value.contains("FAIL"))
            qDebug() << "Scanning fails";
        actionsQueue.enqueue(actionStatus);
        break;
    case SCAN_RESULT:
        devices = value.split("\n");
        if (devices.length() > 2) {
            devices.removeFirst(); devices.removeLast();
            emit devicesFounded(devices);
        }
    case CHANGE_INTENT:
        if (value.contains("FAIL"))
            qDebug() << "Change intent fails";
    case CHANGE_CHANNEL:
    default: break;
    }

    currentAction = NONE;
    mutex.unlock();
}

void WPAp2p::run()
{
    while (1) {
        sleep(1.5);
        mutex.lock();
        if (currentAction == NONE) {
            if (!actionsQueue.isEmpty()) {
                ActionValue action = actionsQueue.dequeue();
                currentAction = action.action;

                switch (action.action) {
                case CHANGE_CHANNEL:
                    qDebug() << "channel: " << action.value;
                    break;
                case CHANGE_INTENT:
                    WPAProcess.write(QString(SET_COMMAND).arg("p2p_go_intent").
                                     arg(action.value).toAscii());
                    break;
                case GETTING_STATUS:
                    WPAProcess.write(GET_STATUS);
                    break;
                case SCANNING:
                    WPAProcess.write(QString(P2P_FIND).arg(TIMEOUT).
                                     toAscii());
                    QTimer::singleShot(TIMEOUT, this, SLOT(getPeers()));
                    break;
                case SCAN_RESULT:
                    WPAProcess.write(GET_PEERS);
                    break;
                case START_GROUP:
                    WPAProcess.write(CREATE_GROUP);
                    break;
                case STOP_GROUP:
                    WPAProcess.write(QString(REMOVE_GROUP).
                                     arg("wlan0").toAscii());
                    break;
                default:
                    break;
                }
            }
        }
        mutex.unlock();
    }
}

void WPAp2p::scan()
{
    if (WPAPid == -1) return;

    ActionValue action = {SCANNING, 0};
    actionsQueue.enqueue(action);

    QTimer::singleShot(TIMEOUT, this, SLOT(getPeers()));
}

void WPAp2p::setChannel(int value)
{
    if (WPAPid == -1) return;

    ActionValue action = {CHANGE_CHANNEL, value};
    actionsQueue.enqueue(action);
}

void WPAp2p::setIntent(int value)
{
    if (WPAPid == -1) return;

    ActionValue action = {CHANGE_INTENT, value};
    actionsQueue.enqueue(action);
}

void WPAp2p::start(Priority priority)
{
    WPAProcess.start("wpa_cli");
    if (!WPAProcess.waitForStarted(3000))
        return;

    ActionValue action = {SCANNING, 0};
    actionsQueue.enqueue(action);

    QTimer::singleShot(TIMEOUT, this, SLOT(getPeers()));
    QThread::start(priority);
}

void WPAp2p::startGroup()
{
    if (WPAPid == -1) return;

    ActionValue action;
    if (hasGroup)
        action.action = STOP_GROUP;
    else
        action.action = START_GROUP;

    actionsQueue.enqueue(action);
}

void WPAp2p::setEnabled(bool state)
{
    if (state) {
        if (WPAPid != -1)
            return;
        if (QProcess::startDetached(WPA_PROCESS_NAME,
                                    QStringList() << "d" << "-Dnl80211"
                                    << "-c/etc/wpa_supplicant.conf" << "-iwlan0"
                                    << "-B", QDir::rootPath(), &WPAPid)) {
            emit enabled(true);
            WPAPid += 1;        // It's really weird, but startDetached is
                                // it's always returning the pid - 1.
            this->sleep(5);     // waiting the wpa_cli reconnects.
            ActionValue actionStatus = {GETTING_STATUS, 0};
            actionsQueue.enqueue(actionStatus);
        } else {
            emit enabled(false);
        }
    } else {
        if (WPAPid == -1)
            return;
        if (kill(WPAPid, SIGKILL) != -1) {
            WPAPid = -1;
            emit enabled(false);
            actionsQueue.clear();
            currentAction = NONE;
        }
    }
}
