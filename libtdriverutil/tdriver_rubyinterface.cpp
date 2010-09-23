/***************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (testabilitydriver@nokia.com)
**
** This file is part of Testability Driver.
**
** If you have questions regarding the use of this file, please contact
** Nokia at testabilitydriver@nokia.com .
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include <cstdlib>

#include "tdriver_rubyinterface.h"

#include "tdriver_util.h"

#include <QMap>
#include <QByteArray>
#include <QList>
#include <QFile>
#include <QDebug>
#include <QTcpSocket>
#include <QHostAddress>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QMutex>


#include "tdriver_debug_macros.h"


static const char delimChar = 032; // note: octal
static const char InteractDelimCstr[] = { delimChar, delimChar, 0 };


// debug macros
#define VALIDATE_THREAD (Q_ASSERT(validThread == QThread::currentThread()))
#define VALIDATE_THREAD_NOT (Q_ASSERT(validThread != QThread::currentThread()))


TDriverRubyInterface::TDriverRubyInterface() :
        QThread(NULL),
        rbiPort(0),
        rbiVersion(0),
        rbiTDriverVersion(),
        syncMutex(new QMutex()),
        msgCond(new QWaitCondition()),
        helloCond(new QWaitCondition()),
        process(NULL),
        conn(NULL),
        handler(NULL),
        initState(Closed),
        stderrEvalSeqNum(0),
        stdoutEvalSeqNum(0),

        delimStr(InteractDelimCstr),
        evalStartStr(delimStr + "START "),
        evalEndStr(delimStr + "END "),

        validThread(NULL)
{
}


TDriverRubyInterface::~TDriverRubyInterface()
{
    delete msgCond;
    delete helloCond;
    delete syncMutex;
}


void TDriverRubyInterface::startGlobalInstance()
{
    qDebug() << FFL;
    Q_ASSERT (!pGlobalInstance);

    pGlobalInstance = new TDriverRubyInterface();
    pGlobalInstance->moveToThread(pGlobalInstance);
    pGlobalInstance->setValidThread(pGlobalInstance->thread());
    connect(pGlobalInstance, SIGNAL(requestRubyConnection(int)), pGlobalInstance, SLOT(resetRubyConnection(int)), Qt::QueuedConnection);
    connect(pGlobalInstance, SIGNAL(requestCloseSignal()), pGlobalInstance, SLOT(close()), Qt::QueuedConnection);

    pGlobalInstance->start();
}


void TDriverRubyInterface::requestClose()
{
    VALIDATE_THREAD_NOT;
    qDebug() << FCFL;
    emit requestCloseSignal();
}


void TDriverRubyInterface::run()
{
    // TODO: change TDriverRubyInterface to inherit from QObject,
    // override moveToThread and move setValidThread there,
    // and use plain QThread object as the thread,
    // and maybe have startGlobalInstance(QThread *thread)
    qDebug() << FCFL << "THREAD START";
    setValidThread(currentThread());

    int result = exec();
    qDebug() << FCFL << "THREAD EXIT with" << result;

    if (handler) { delete handler; handler = NULL; }
    if (conn) { delete conn; conn = NULL; }

    delete process; // will kill the process
    process = NULL;
}


bool TDriverRubyInterface::goOnline()
{
    VALIDATE_THREAD_NOT;
    qDebug() << FCFL << "entry in initstate" << initState;
    Q_ASSERT(isRunning()); // must only be called when thread is running
    QMutexLocker lock(syncMutex);
    if (initState == Closed) {
        static int counter=0;
        ++counter;
        qDebug() << FCFL << "Emitting requestrubyconnection #" << counter;
        emit requestRubyConnection(counter);
        if (!msgCond->wait(syncMutex, 80*1000)) {
            qWarning() << "Request to starting ruby process failed unexpectedly!";
        }
    }

    if (initState == Running) {
        if (!handler->isHelloReceived()) {
            // now there should be a TCP connection, but no messages received yet
            qDebug() << FCFL << "Waiting for HELLO";
            bool ok = handler->waitHello(5000);
            qDebug() << FCFL << "after waitHello:" << ok << handler->isHelloReceived();
            if (!handler->isHelloReceived()) {
                qWarning() << "Ruby script tdriver_interface.rb did not say hello to us (initState" << initState << "), closing.";
                requestClose();
            }
            else {
                qDebug() << FCFL << "Running -> Connected after hello wait";
                initState = Connected;
            }
        }
        else {
            qDebug() << FCFL << "Running -> Connected as hello already received";
            initState = Connected;
        }
    }

    qDebug() << FCFL << "return in initstate" << initState << ", connected" << (initState == Connected);
    return (initState == Connected);
}


void TDriverRubyInterface::recreateConn()
{
    VALIDATE_THREAD;
    qDebug() << FCFL;
    if (handler) {
        delete handler;
        handler = NULL;
    }

    // reset or create QTcpSocket instance
    if (conn) {
        if (conn->isOpen()) {
            conn->close();
            if (conn->bytesAvailable() > 0) {
                QByteArray tmp = conn->readAll();
                qDebug() << FCFL << tmp.size() << "bytes";
            }
        }
    }
    else {
        conn = new QTcpSocket(this);
    }
    Q_ASSERT(conn && conn->state() ==QAbstractSocket::UnconnectedState && conn->bytesAvailable() == 0);
}


void TDriverRubyInterface::resetProcess()
{
    VALIDATE_THREAD;
    Q_ASSERT(process);
    Q_ASSERT(initState == Closing);

    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(5000)) {
            process->kill();
            if (!process->waitForFinished(5000)) {
                qFatal("Failed to kill ruby process!");
            }
        }
    }
    // else aready not running

    qDebug() << FCFL << "reporting exitcode" << process->exitCode() << "exitstatus" << process->exitStatus();
    process->disconnect(); // disconnect any stray signals
    readProcessStdout();
    readProcessStderr();
    initState = Closed;
}

void TDriverRubyInterface::recreateProcess()
{
    VALIDATE_THREAD;
    qDebug() << FCFL;
    // reset or create QProcess instance
    if (process) {
        initState = Closing;
        resetProcess();
        Q_ASSERT(initState == Closed);
    }
    else {
        qDebug() << FCFL;
        process = new QProcess(this);

        // set RUBYOPT env. setting if system doesn't have it already
        QStringList envSettings = QProcess::systemEnvironment();
        if ( QString( getenv( "RUBYOPT" ) ) != "rubygems" ) { envSettings << "RUBYOPT=rubygems"; }
        process->setEnvironment( envSettings );
    }

    Q_ASSERT(process && process->state() == QProcess::NotRunning && process->bytesAvailable() == 0);

    // (re)connect signals
    qDebug() << FCFL;

    connect(process, SIGNAL(finished(int,QProcess::ExitStatus)), this, SIGNAL(rubyProcessFinished()));
    qDebug() << FCFL;
}


void TDriverRubyInterface::resetRubyConnection(int counter)
{
    VALIDATE_THREAD;
    qDebug() << FCFL << "#" << counter;

    QMutexLocker lock(syncMutex);
    recreateConn();
    recreateProcess();

    bool ok = true;
    initErrorMsg.clear();
    QString errorTitle(tr("Failed to initialize TDriver"));

    // if TDRIVER_VISUALIZER_LISTENER environment variable is set, use a custom file to use as the listener
    QString scriptFile = TDriverUtil::tdriverHelperFilePath("tdriver_interface.rb", "TDRIVER_VISUALIZER_LISTENER");

    if (ok) {
        // verify that script file exists
        if (!QFile::exists( scriptFile )) {
            initErrorMsg = tr("Could not find Visualizer listener server file '%1'" ).arg(scriptFile);
            qDebug() << FCFL << "emit error" << errorTitle << initErrorMsg;
            emit error(errorTitle, initErrorMsg, "");
            ok = false;
        }
    }

    if (ok) process->setTextModeEnabled(true);

    if (ok) process->start( "ruby", QStringList() << scriptFile );

    if ( ok && !process->waitForStarted( 20000 ) ) {
        initErrorMsg = tr("Could not start Ruby script '%1'" ).arg(scriptFile);
        qDebug() << FCFL << "emit error" << errorTitle << initErrorMsg;
        emit error(errorTitle, initErrorMsg, "");
        ok = false;
    }

    if ( ok && !process->waitForReadyRead(40000)) {
        initErrorMsg = tr("Could not read startup parameters." );
        qDebug() << FCFL << "emit error" << errorTitle << initErrorMsg;
        emit error(errorTitle, initErrorMsg, "");
        ok = false;
    }

    if ( ok && !process->canReadLine()) {
        initErrorMsg = tr("Could not read full line of." );
        qDebug() << FCFL << "emit error" << errorTitle << initErrorMsg;
        emit error(errorTitle, initErrorMsg, "");
        ok = false;
    }

    QByteArray startupLine;
    if (ok) {
        startupLine = process->readLine().simplified();
        BAList startupList(startupLine.split(' '));

        // Ruby string printed at script startup:
        // "TDriverVisualizerRubyInterface version #{tdriver_interface_rb_version} port #{server.addr[1]} tdriver #{tdriver_gem_version}"
        if (startupList.length() < 7 ||
            startupList[0] != "TDriverVisualizerRubyInterface" ||
            startupList[1] != "version" ||
            startupList[2].toInt() == 0 ||
            startupList[3] != "port" ||
            startupList[4].toInt() == 0 ||
            startupList[5] != "tdriver" ||
            startupList[6].isEmpty())
        {
            initErrorMsg = tr("Invalid first line '%1'.").arg(QString::fromLocal8Bit(startupLine));
            qDebug() << FCFL << "emit error" << errorTitle << initErrorMsg;
            emit error(errorTitle, initErrorMsg, tr("More ouput:\n") + process->readAllStandardOutput());
            ok = false;
        }
        else {
            rbiVersion = startupList[2].toInt();
            rbiPort = startupList[4].toInt();
            rbiTDriverVersion = startupList[6];
        }
    }

    if (ok && ((rbiPort < 1 || rbiPort > 65535) || rbiVersion != 1)) {
        initErrorMsg = tr("Invalid values on first line: rbiPort %1, rbiVersion %2").arg(rbiPort).arg(rbiVersion);
        qDebug() << FCFL << "emit error" << errorTitle << initErrorMsg;
        emit error(errorTitle, initErrorMsg, tr("More ouput:\n") + process->readAllStandardOutput());
        ok = false;
    }

    connect(process, SIGNAL(readyReadStandardError()), this, SLOT(readProcessStderr()));
    connect(process, SIGNAL(readyReadStandardOutput()), this, SLOT(readProcessStdout()));
    // read and ignore any extra output
    readProcessStderr();
    readProcessStdout();

    if (ok) {
        Q_ASSERT(!handler);
        handler = new TDriverRbiProtocol(conn, syncMutex, msgCond, helloCond, this);
        handler->setValidThread(currentThread());
        connect(handler, SIGNAL(helloReceived()), this, SIGNAL(rubyOnline()));
        connect(handler, SIGNAL(messageReceived(quint32,QByteArray,BAListMap)), this, SIGNAL(messageReceived(quint32,QByteArray,BAListMap)));
        connect(handler, SIGNAL(gotDisconnection()), this, SLOT(close()));
        qDebug() << FCFL << "Connecting localhost :" << rbiPort;
        conn->connectToHost(QHostAddress(QHostAddress::LocalHost), rbiPort);
        if (!conn->waitForConnected(30000)) {
            initErrorMsg = tr("Failed to connect to Ruby process via TCP/IP!");
            qDebug() << FCFL << "emit error" << errorTitle << initErrorMsg;
            emit error(errorTitle, initErrorMsg, "");
            ok = false;
        }
    }

    if (ok) {
        initState = Running;
    }
    qDebug() << FCFL << "RESULT" << ok << initState;

    // Notify the thread that called resetRubyConnection
    msgCond->wakeAll();


    if (!ok) {
        helloCond->wakeAll();
    }
}


void TDriverRubyInterface::close()
{
    VALIDATE_THREAD;

    // This method will be called from just one thread,
    // which is guaranteed by VALIDATE_THREAD above.
    // Therefore it's ok to have alreadyClosing flag without mutex

    static bool alreadyClosing = false;
    if (alreadyClosing) return;
    alreadyClosing = true;

    syncMutex->lock();


    if (initState == Closed || initState == Closing) {
        qDebug() << FCFL << "initState already" << initState;
    }
    else {
        initState = Closing;

        msgCond->wakeAll();
        helloCond->wakeAll();

        qDebug() << FCFL << "TDriverRubyInterface: Closing process, process state" << process->state() << ", conn state" << conn->state();
        if (conn->isOpen()) {
            conn->close();
        }
        resetProcess();
        Q_ASSERT(initState == Closed);
    }
    syncMutex->unlock();
    alreadyClosing = false;

}


void TDriverRubyInterface::readProcessHelper(int fnum, QByteArray &readBuffer, quint32 &seqNum, QByteArray &evalBuffer)
{
    QByteArray data( (fnum == 0)
                    ? process->readAllStandardOutput()
                        : process->readAllStandardError());

    const char *streamName = (fnum == 0) ? "STDOUT" : "STDERR";

    readBuffer.append(data);
    BAList lines = readBuffer.split('\n');
    readBuffer = lines.takeLast(); // put empty or partial line back to buffer

    foreach(QByteArray line, lines) {
        if (line.startsWith(delimStr)) {
            if (seqNum > 0) {
                qDebug() << FCFL << streamName << "seqNum" << seqNum << "output" << evalBuffer;
                emit rubyOutput(fnum, seqNum, evalBuffer);
            }
            evalBuffer.clear();
            seqNum = 0;

            if (line.startsWith(evalStartStr)) {
                static const QRegExp start_ex("START ([0-9]+)");
                if (start_ex.indexIn(QString(line)) > 0) {
                    seqNum = start_ex.capturedTexts().at(1).toUInt();
                }
                else {
                    qWarning() << FCFL << "invalid start line" << line;
                }
            }
            else if (line.startsWith(evalEndStr)) {
                // nothing
            }
            else {
                qDebug() << FCFL << streamName << "IGNORING" << line;
            }
        }
        else if (seqNum > 0) {
            evalBuffer.append(line);
            evalBuffer.append('\n');
        }
        else {
            qDebug() << FCFL << streamName << "untagged line" << line;
            emit rubyOutput(fnum, line);
        }
    }
}


void TDriverRubyInterface::readProcessStdout()
{
    VALIDATE_THREAD;
    readProcessHelper(0, stdoutBuffer, stdoutEvalSeqNum, stdoutEvalBuffer);
}


void TDriverRubyInterface::readProcessStderr()
{
    VALIDATE_THREAD;
    readProcessHelper(1, stderrBuffer, stderrEvalSeqNum, stderrEvalBuffer);
}


quint32 TDriverRubyInterface::sendCmd( const QByteArray &name, const BAListMap &cmd)
{
    if (initState != Connected || !handler) {
        return 0;
    }

    quint32 seqNum = handler->sendStringListMapMsg(name, cmd);
    Q_ASSERT(seqNum > 0);
    return seqNum;
}

bool TDriverRubyInterface::executeCmd(const QByteArray &name, BAListMap &cmd_reply, unsigned long timeout)
{
    VALIDATE_THREAD_NOT;
    if (!goOnline()) return false;

    QMutexLocker lock(syncMutex);
    qDebug() << FCFL << "SENDING" << cmd_reply;
    quint32 seqNum = sendCmd(name, cmd_reply);
    //qDebug() << FCFL << "Sent" << seqNum << name << cmd_reply;
    if (seqNum != 0) {

        if (handler->waitSeqNum(seqNum, timeout)) {
            cmd_reply = handler->waitedMessage();
            // TODO: make final decision about which logic to use here, and change ruby_interface.rb accordingly:
#if 1
            if (cmd_reply.contains("error") && cmd_reply.value("error").isEmpty()) cmd_reply["error"] << "Unknown error";
#else
            if (cmd_reply.contains("error") && cmd_reply.value("error").isEmpty()) cmd_reply.remove("error");
#endif
            qDebug() << FCFL << "REPLY" << cmd_reply;
            return true;
        }
    }
    qDebug() << FCFL << "FAIL";
    cmd_reply.clear();
    cmd_reply["error"] << "Error: Timeout waiting for TDriver interface script";
    return false;
}


int TDriverRubyInterface::getPort()
{
    VALIDATE_THREAD_NOT;
    QMutexLocker lock(syncMutex);
    return rbiPort;
}


int TDriverRubyInterface::getRbiVersion()
{
    VALIDATE_THREAD_NOT;
    QMutexLocker lock(syncMutex);
    return rbiVersion;
}


QString TDriverRubyInterface::getTDriverVersion()
{
    VALIDATE_THREAD_NOT;
    QMutexLocker lock(syncMutex);
    return rbiTDriverVersion;
}


TDriverRubyInterface *TDriverRubyInterface::globalInstance()
{
    //VALIDATE_THREAD_NOT;
    //Q_ASSERT(pGlobalInstance);
    return pGlobalInstance;
}


TDriverRubyInterface *TDriverRubyInterface::pGlobalInstance = NULL;

