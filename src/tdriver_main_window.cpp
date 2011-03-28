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


#include "tdriver_main_window.h"
#include "tdriver_recorder.h"
#include "tdriver_image_view.h"

#include <tdriver_tabbededitor.h>
#include <tdriver_rubyinterface.h>
#include "../common/version.h"


#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QErrorMessage>
#include <QThread>
#include <QToolBar>
#include <QLabel>
#include <QTimer>

#include <tdriver_debug_macros.h>


MainWindow::MainWindow() :
    QMainWindow(),
    foregroundApplication(false),
    tdriverMsgBox(new QErrorMessage(this)),
    tdriverMsgTotal(0),
    tdriverMsgShown(1),
    keyLastUiStateDir("files/last_uistate_dir"),
    keyLastTDriverDir("files/last_tdriver_dir"),
    messageTimeoutTimer(new QTimer(this))
{
    resetMessageSequenceFlags();
    messageTimeoutTimer->setSingleShot(true);
    connect(messageTimeoutTimer, SIGNAL(timeout()), SLOT(messageTimeoutSlot()));

    // ugly hack to disable the "don't show again" checkbox,
    // may not work in future Qt versions, and may not work on all platforms
    foreach(QObject *child, tdriverMsgBox->children()) {
        QCheckBox *cb = qobject_cast<QCheckBox*>(child);
        if (cb) {
            //qDebug() << FCFL << "hiding tdriverMsgBox QCheckBox" << cb->text();
            cb->hide();
            cb->setDisabled(true);
            cb->setCheckState(Qt::Checked);
        }
        else {
            QPushButton *pb = qobject_cast<QPushButton*>(child);
            if (pb) {
                //qDebug() << FCFL << "connecting tdriverMsgBox QPushButton" << pb->text();
                connect(pb, SIGNAL(clicked()), this, SLOT(tdriverMsgOkClicked()));
            }
        }
    }

    tdriverMsgBox->resize(600, 400);
    tdriverMsgBox->setSizeGripEnabled(true);
    tdriverMsgSetTitleText();
    connect(tdriverMsgBox, SIGNAL(finished(int)), this, SLOT(tdriverMsgFinished()));
}


void MainWindow::tdriverMsgSetTitleText()
{
    tdriverMsgBox->setWindowTitle(tr("TDriver Notification %1/%2").
                                  arg(qMin(tdriverMsgShown, tdriverMsgTotal)).
                                  arg(tdriverMsgTotal));
    //qDebug() << FCFL << "set tdriverMsgBox title to" << tdriverMsgBox->windowTitle();
}


void MainWindow::tdriverMsgOkClicked()
{
    ++tdriverMsgShown;
    tdriverMsgSetTitleText();
    tdriverMsgBox->repaint();
}


void MainWindow::tdriverMsgFinished()
{
    // verify that tdriverMsgShown is not out of sync
    if (tdriverMsgShown < tdriverMsgTotal) {
        tdriverMsgShown = tdriverMsgTotal;
        tdriverMsgSetTitleText();
    }
}


void MainWindow::tdriverMsgAppend(QString message)
{
    message.replace('\n', QChar::ParagraphSeparator);
    ++tdriverMsgTotal;
    tdriverMsgSetTitleText();
    tdriverMsgBox->showMessage(message);
    tdriverMsgBox->raise();
    tdriverMsgBox->repaint();
}

// Performs post-initialization checking.
void MainWindow::checkInit()
{
    // check if RUBYOPT has been set
    QString current( getenv( tr( "RUBYOPT" ).toLatin1().data() ) );
    if ( current != "rubygems" ) { putenv( tr( "RUBYOPT=rubygems" ).toLatin1().data() ); }
}


bool MainWindow::checkVersion( QString currentVersion, QString requiredVersion )
{
    // convert required version to array
    QStringList tmpRequiredVersionArray = requiredVersion.split( QRegExp("[.-]") );

    // convert installed driver version to array
    QStringList tmpDriverVersionArray = currentVersion.split( QRegExp("[.-]") );

    // make version arrays same length
    while ( tmpDriverVersionArray.count() < tmpRequiredVersionArray.count() ) {
        tmpDriverVersionArray.append("0");
    }

    bool versionOk = true;

    for ( int index = 0; index < tmpRequiredVersionArray.count();  index++ ) {
        int current_version = tmpDriverVersionArray.at( index ).toInt();
        int required_version = tmpRequiredVersionArray.at( index ).toInt();

        // check if installed version is new enough
        if ( current_version > required_version ) {
            break;
        }
        else {
            if ( current_version < required_version ){
                versionOk = false;
                break;
            }
        }
    }

    return versionOk;
}


// Function entered when creating Visulizer. Sets default and calls helper functions
// to setup UI of Visualizer
bool MainWindow::setup()
{
    setObjectName("main");
    QTime t;  // for performance debugging, can be removed

    QSettings settings;

    TDriverRubyInterface::startGlobalInstance();

    connect(TDriverRubyInterface::globalInstance(), SIGNAL(rbiError(QString,QString,QString)),
            SLOT(handleRbiError(QString,QString,QString)));

    connect(TDriverRubyInterface::globalInstance(), SIGNAL(messageReceived(quint32,QByteArray,BAListMap)),
            SLOT(receiveTDriverMessage(quint32,QByteArray,BAListMap)));

    // determine if connection to TDriver established -- if not, allow user to run TDriver Visualizer in viewer/offline mode
    offlineMode = true;
    t.start();
    QString goOnlineError;
    if (!(goOnlineError = TDriverRubyInterface::globalInstance()->goOnline()).isNull()) {
        tdriverMsgAppend(tr("TDriver Visualizer failed to interface with TDriver framework:\n\n" )
                         + goOnlineError
                         + tr("\n\n=== Launching in offline mode ==="));
    }
    else {
        QString installedDriverVersion = getDriverVersionNumber();

        if ( !checkVersion( installedDriverVersion, REQUIRED_DRIVER_VERSION ) ) {
            tdriverMsgAppend(tr("TDriver Visualizer is not compatible with this version of TDriver. Please update your TDriver environment.\n\n") +
                             tr("Installed version: ") + installedDriverVersion +
                             tr("\nRequired version: ") + REQUIRED_DRIVER_VERSION + tr(" or later")+
                             tr("\n\n=== Launching in offline mode ===")
                             );
        }
        else {
            offlineMode = false; // TDriver successfully initialized!
        }
    }
    qDebug() << FCFL << "RBI goOnline  result" << !offlineMode << "secs" << float(t.elapsed())/1000.0;

    if (offlineMode) {
        qWarning("Failed to initialize TDriver, closing Ruby process");
        TDriverRubyInterface::globalInstance()->requestClose();
    }

    // default font for QTableWidgetItems and QTreeWidgetItems
    defaultFont = new QFont;
    defaultFont->fromString(  settings.value( "font/settings", QString("Sans Serif,8,-1,5,50,0,0,0,0,0") ).toString() );
    emit defaultFontSet(*defaultFont);

    // default window size & position
    QPoint windowPosition = settings.value( "window/pos", QPoint( 200, 200 ) ).toPoint();
    QSize windowSize = settings.value( "window/size", QSize( 950, 600 ) ).toSize();

    // read dock visibility settings
    bool imageVisible = settings.value( "view/image", true ).toBool();
    bool clipboardVisible = settings.value( "view/clipboard", true ).toBool();
    bool propertiesVisible = true; //settings.value( "view/properties", true ).toBool();
#if DEVICE_BUTTONS_ENABLED
    bool buttonVisible = settings.value( "view/buttons", false ).toBool();
#endif
    bool shortcutsVisible = settings.value( "view/shortcuts", false ).toBool();
    bool appsVisible = settings.value( "view/applications", false ).toBool();
    bool editorVisible = settings.value( "view/editor", false ).toBool();

    // default sut
    QString defaultDevice = settings.value( "sut/activesut", QString( "sut_qt" ) ).toString();

    // if default sut is empty (in ini file), set it as 'sut_qt'
    if ( defaultDevice.isEmpty() ) { defaultDevice = "sut_qt"; }

    // initialize & default settings

    tdriverPath = settings.value( "files/location", "" ).toString();
    if ( tdriverPath.isEmpty() ) {
        // construct QString tdriverPath depending on OS
#if (defined(Q_OS_WIN32))
        tdriverPath = "c:/tdriver/";
#else
        tdriverPath = "/etc/tdriver/";
#endif
    }

    // set current parameters xml file to be used
    while ( !QFile((parametersFile = tdriverPath + "/tdriver_parameters.xml")).exists() ) {

        QMessageBox::StandardButton result = QMessageBox::critical(
                    0,
                    tr("Missing file"),
                    tr("Could not locate TDriver parameters file:\n\n  %1\n\n").arg(parametersFile) +
                    tr("Please click Ok to select correct folder, or Cancel to quit.\n\nNote: Location will be saved to Visualizer configuration."),
                    QMessageBox::Ok | QMessageBox::Cancel);

        if (result & QMessageBox::Cancel) {
            return false; // exit
        }

        tdriverPath = selectFolder( "Select folder of TDriver default tdriver_parameters.xml file", "Folder", QFileDialog::AcceptOpen, keyLastTDriverDir);
        if (!tdriverPath.endsWith('/')) tdriverPath += '/';
    }
    settings.setValue( "files/location", tdriverPath );

    // object tree
    collapsedObjectTreeItemPtr = 0;
    expandedObjectTreeItemPtr = 0;
    lastHighlightedObjectKey = 0;

    /* initialize help, context sensitivity needs watch for events */
    installEventFilter( this );

    // create tdriver recorder
    mRecorder = new TDriverRecorder( this );

    // create show xml dialog
    createXMLFileDataWindow();

    createActions();

    // create user interface
    createUi();

    // create find dialog
    createFindDialog();

    // create start app dialog
    createStartAppDialog();

    // parse parameters xml to retrieve all devices
    if ( !offlineMode ){
        offlineMode = !getXmlParameters( parametersFile );

        if ( !offlineMode && deviceList.count() > 0 ) {
            deviceMenu->setEnabled( true );
            disconnectCurrentSUT->setEnabled( true );
            delayedRefreshAction->setEnabled( true );
            parseSUT->setEnabled( true );
        }
        tabEditor->setTDriverParamMap(tdriverXmlParameters);
    }

    // default sut
    setActiveDevice( defaultDevice );

    // update main window title - current sut will be shown
    updateWindowTitle();

    // set visibilities
    clipboardBar->setVisible( clipboardVisible );
    imageViewDock->setVisible( imageVisible );
    propertiesDock->setVisible( propertiesVisible );
#if DEVICE_BUTTONS_ENABLED
    keyboardCommandsDock->setVisible( buttonVisible );
#endif
    shortcutsBar->setVisible( shortcutsVisible );
    appsBar->setVisible( appsVisible );
    editorDock->setVisible( editorVisible );

    // resize window
    resize( windowSize );
    move( windowPosition );

    connectSignals();

    // xml/screen capture output path depending on OS
    outputPath = QDir::tempPath();
#if (defined(Q_OS_WIN32))
    outputPath = QString( getenv( "TEMP" ) ) + "/";
#else
    outputPath = "/tmp/";
#endif

    if ( !offlineMode &&
            !executeTDriverCommand( commandSetOutputPath, "listener set_output_path " + outputPath) ) {
        outputPath = QApplication::applicationDirPath();
    }

    deviceSelected();
    return true;

} // setup


void MainWindow::setActiveDevice(const QString &deviceName )
{
    if ( !deviceName.isEmpty() && deviceList.contains( deviceName ) ) {
        activeDevice = deviceName;
        getActiveDeviceParameters();

        // enable recording menu if device type is 'kind of' qt
        recordMenu->setEnabled( !applicationsNamesMap.empty()
                               && TDriverUtil::isQtSut(activeDeviceParams.value( "type" )));
    }
    else {
        activeDevice.clear();
        activeDeviceParams.clear();
    }
    tabEditor->setSutParamMap(activeDeviceParams);
}


QString MainWindow::getDriverVersionNumber()
{
#if 1
    QString ver(TDriverRubyInterface::globalInstance()->getTDriverVersion());
    qDebug() << FCFL << "got version" << ver;
    return  (ver.isEmpty()) ? "Unknown" : ver;
#else
    QByteArray result = "Unknown";
    BAListMap reply;

    if ( executeTDriverCommand(commandGetVersionNumber, "listener check_version", "", &reply ) ) {
        result = cleanDoneResult(reply["OUTPUT"].first());
    }

    qDebug() << FCFL << "GOT VERSION" << result;
    return QString(result);
#endif
}

QByteArray MainWindow::cleanDoneResult(QByteArray output)
{
    int doneIndex = output.indexOf("done");
    if ( doneIndex > -1 ) {
        // remove done from the end of string
        output.truncate(doneIndex);
    }
    // remove linefeed characters
    return output.trimmed();
}


QString MainWindow::getDeviceParameter( QString deviceName, QString parameter )
{
    qDebug() << FCFL << deviceName;
    QByteArray result = "Unknown";
    BAListMap reply;
    QString command = deviceName + " get_parameter " + parameter;

    if ( executeTDriverCommand( commandGetDeviceParameter, command, deviceName, &reply ) ) {
        if (reply.contains("parameter")) {
            if (reply.value("parameter").size() == 2 && reply.value("parameter").at(0) == parameter) {
                result = reply.value("parameter").at(1);
            }
            else qDebug() << "BAD get_parameter VALUE" << reply.value("parameters");
        }
        else qDebug() << "BAD get_parameter REPLY" << reply;
    }
    else qDebug() << "FAILED get_parameter";

    qDebug() << FCFL << "got" << result;
    return QString(result);
}


bool MainWindow::getActiveDeviceParameters()
{
    qDebug() << FCFL << activeDevice;
    BAListMap reply;
    QString command = activeDevice + " get_all_parameters";

    bool result = false;
    if ( executeTDriverCommand( commandGetAllDeviceParameters, command, activeDevice, &reply ) ) {
        const BAList &keys = reply["keys"];
        const BAList &values = reply["values"];
        int count = keys.count();
        if (count > 0 && count == values.count() ) {
            activeDeviceParams.clear();
            result = true;
            for (int ii=0; ii < count; ++ii) {
                activeDeviceParams.insert(keys.at(ii), values.at(ii));
            }
            //qDebug() << FCFL << activeDevice << ":" << activeDeviceParams;
        }
        else qDebug() << FCFL << "BAD get_parameter keys and/or values counts:" << keys.count() << values.count();
    }
    else qDebug() << FCFL << "FAILED get_all_parameters" << activeDevice;

    qDebug() << FCFL << "got" << result;
    return result;
}


void MainWindow::connectSignals()
{
    connectObjectTreeSignals();
    connectTabWidgetSignals();
    connectImageWidgetSignals();

    QMetaObject::connectSlotsByName( this );
}


// This is called when closing Visualizer. Call threads close method, which
// shuts down thread (closes and then kills it)
void MainWindow::closeEvent( QCloseEvent *event )
{
    QSettings settings;

    if (tabEditor && !tabEditor->mainCloseEvent(event)) {
        qDebug() << "closeEvent rejected by code editor";
        event->ignore();
        return;
    }

    TDriverRubyInterface::globalInstance()->requestClose();

    // close show xml window if still visible
    //if ( xmlView->isVisible() ) { xmlView->close();    }

    // save tdriver path
    settings.setValue( "files/location", tdriverPath );

    // default window size & position
    settings.setValue("window/pos", pos());
    settings.setValue("window/size", size());

    // default sut
    settings.setValue("sut/activesut", activeDevice);
    settings.remove("sut/activesuttype"); // this is no longer stored in application settings

    // object tree settings
    //for ( int i = 0; i < 3 ; i++ ) { settings.setValue( QString("objecttree/column" + QString::number( i ) ), objectTree->columnWidth( i ) ); }

    // image settings
    settings.setValue("image/resize", checkBoxResize->isChecked());

    // clipboard contents

    // view visiblity settings
    settings.setValue( "view/clipboard", clipboardBar->isVisible());
    settings.setValue( "view/applications", appsBar->isVisible());
    settings.setValue( "view/image", imageViewDock->isVisible());
    settings.setValue( "view/properties", propertiesDock->isVisible() );
#if DEVICE_BUTTONS_ENABLED
    settings.setValue( "view/buttons", viewButtons->isChecked() );
#else
    settings.remove("view/buttons");
#endif
    settings.setValue( "view/shortcuts", shortcutsBar->isVisible());
    settings.setValue( "view/editor", editorDock->isVisible());

    settings.setValue( "font/settings", defaultFont->toString() );
}

// Event filter, catches F1/HELP key events and processes them
bool MainWindow::eventFilter(QObject * object, QEvent *event) {

    Q_UNUSED( object );

    if (event->type() == QEvent::KeyPress) {

        QKeyEvent *ke = static_cast<QKeyEvent *>(event);

        //qDebug() << "modifiers(): " << ke->modifiers() << ", key(): " << ke->key();

        if (ke->key() == Qt::Key_F1 || ke->key() == Qt::Key_Help) {

            //QWidget *widget = 0;
            QString page = "qdoc-temp/index.html";
#if 0
            // Context sensitivity disabled for now
            // You need to also remove the line "visualizerAssistant->setShortcut(tr("F1"));"
            // from the createTopMenuBar method to enable processing of F1 ket events in this event handler
            if (object->isWidgetType()) {

                widget = static_cast<QWidget *>(object)-> focusWidget();
                if (widget == objectTree)
                    page = "tree.html";
            }else {
                page = "devices.html";
            }
#endif
            showContextVisualizerAssistant(page);

            return true;
        }
    }
    return QMainWindow::eventFilter(object, event);
}


// MainWindow listener for keypresses
void MainWindow::keyPressEvent ( QKeyEvent * event )
{
    // qDebug() << "MainWindow::keyPressEvent: " << event->key();
    if ( QApplication::focusWidget() == objectTree && objectTree->currentItem() != NULL )
        objectTreeKeyPressEvent( event );
    else
        event->ignore();

    collapsedObjectTreeItemPtr = 0;
    expandedObjectTreeItemPtr = 0;
}


bool MainWindow::isDeviceSelected()
{
    return !activeDevice.isEmpty();
}


void MainWindow::noDeviceSelectedPopup()
{
    if ( !offlineMode ){
        QMessageBox::critical(0, tr( "Error" ), "Unable to refresh due to no device selected.\n\nPlease select one from devices menu." );
    }
}


QString MainWindow::selectFolder(QString title, QString filter, QFileDialog::AcceptMode mode, const QString &saveDirKey)
{
    QFileDialog dialog( this );
    QSettings settings;

    dialog.setAcceptMode( mode );
    dialog.setFileMode( QFileDialog::Directory );
    dialog.setNameFilter( filter );
    dialog.setWindowTitle( title );
    dialog.setViewMode( QFileDialog::List );
    if (!saveDirKey.isEmpty()) {
        dialog.setDirectory(settings.value(saveDirKey, QVariant("")).toString());
    }

    QString ret;
    if (dialog.exec()) {
        QString dirName = dialog.selectedFiles().at(0);
        if (!dirName.isEmpty()) {
            if (!saveDirKey.isEmpty()) {
                settings.setValue(saveDirKey, dirName);
            }
            ret = dirName;
        }
    }
    return ret;
}


void MainWindow::statusbar( QString text, int timeout )
{
    statusBar()->showMessage( text, timeout );
    statusBar()->repaint();
    qApp->processEvents();
}


void MainWindow::statusbar( QString text, int currentProgressValue, int maxProgressValue, int timeout )
{
    int progress = int( (  float( currentProgressValue ) / float( maxProgressValue ) ) * float( 100 ) );
    statusBar()->showMessage( text + " " + QString::number( progress ) + "%", timeout );
    statusBar()->repaint();
}


void MainWindow::handleRbiError(QString title, QString text, QString details)
{
    tdriverMsgAppend(tr("Error from TDriver interface:\n")
                     + title
                     + "\n____________________\n"
                     + text
                     + "\n____________________\n"
                     + details);
}


void MainWindow::receiveTDriverMessage(quint32 seqNum, QByteArray name, const BAListMap &reply)
{
    if (name != TDriverUtil::visualizationId) return; // not for us

    if (!sentTDriverMsgs.contains(seqNum)) {
        qDebug() << FCFL << "received visualization message with unknown seqNum:" << seqNum;// << reply;
        return;
    }

    qDebug() << FCFL << "received visualization message:" << seqNum << reply;

    SentTDriverMsg sentMsg(sentTDriverMsgs.take(seqNum));

    bool handleError = false;
    bool handleNormally = false;

    if (reply.contains("error")) {
        handleError = true;
        unsigned resultEnum = OK;
        QString clearError, shortError, fullError;
        processErrorMessage(sentMsg.type, "<N/A>", reply, "<unknownn>",
                            resultEnum, clearError, shortError, fullError);
#if 0
        qDebug() << FCFL << "error: " << resultEnum << fullError;
        tdriverMsgAppend(fullError);
        if (resultEnum & RETRY) {
            if (sentMsg.resends <= 0) {
                if (resendTDriverCommand(sentMsg)) {
                    qDebug() << FCFL << "Resending ok";
                    return;
                }
                else {
                    qDebug() << FCFL << "Resend failed!";
                }
            }
            else {
                qDebug() << FCFL << "Ignoring RETRY for resend count" << sentMsg.resends;
            }
        }

        if ( resultEnum & FAIL) {

        }

        if ( resultEnum & DISCONNECT ) {
            // disconnect
            qDebug() << FCFL << "DISCONNECT";

            BAListMap msg;
            msg["input"] << activeDevice.toAscii() << "disconnect";
            /*bool response2 =*/
            TDriverRubyInterface::globalInstance()->executeCmd(TDriverUtil::visualizationId, msg, 9999);
            if (msg.contains("error")) {
                fullError += "\n\nDisconnect after error failed!";
            }
            else {
                fullError += "\n\nDisconnect after error succeeded.";
                // disconnect passed -- retry
                currentApplication.clear();
            }
        }
        //        qDebug() << FCFL << resultEnum << clearError << shortError << fullError;
#else
        qDebug() << FCFL << "Sending disconnect after error:" << resultEnum << fullError;
        statusbar(tr("Sending disconnect after error!"));

        BAListMap msg;
        msg["input"] << activeDevice.toAscii() << "disconnect";
        TDriverRubyInterface::globalInstance()->executeCmd(TDriverUtil::visualizationId, msg, 9999);
        if (msg.contains("error")) {
            fullError += "\n\nDisconnect after error failed!";
            statusbar(tr("Disconnect failed"), 2000);
        }
        else {
            fullError += "\n\nDisconnect after error succeeded.";
            // disconnect passed -- retry
            currentApplication.clear();
            statusbar(tr("Disconnected after error!"), 2000);
        }
        tdriverMsgAppend(fullError);
#endif
        return;
    }
    else if (seqNum > 0) {
        handleNormally = true;
    }


    switch (sentMsg.type) {
    case commandSetOutputPath:
        break;

    case commandListApps:
        if (handleNormally) {
            qDebug() << FCFL << "got app list:" << applicationsNamesMap;
            statusbar(tr("Parsing applications list..."));
            parseApplicationsXml( reply.value("applications_filename").value(0) );
            statusbar(tr("Applications list updated!"), 2000);
        }
        if (doRefreshAfterAppList) {
            // leave doRefreshAfterAppList to true for the call, in case it's used.
            if (!handleError) sendRefreshCommands();
            doRefreshAfterAppList = false;
        }
        break;

    case commandClassMethods:
        break;

    case commandDisconnectSUT:
        qDebug() << FCFL << "Disconnection" << !handleError;
        statusbar(tr("SUT disconnected"), 1000);
        emit disconnectionOk(handleError);
        break;

    case commandRecordingStart:
        break;

    case commandRecordingStop:
        break;

    case commandRecordingTest:
        break;

    case commandTapScreen:
        if (handleNormally && !doRefreshAfterAppList) {
            statusbar(tr("Tap done, auto-refreshing..."), 1000 );
            sendRefreshCommands();
        }
        break;

    case commandRefreshUI:
        if (handleNormally) {
            statusbar(tr("UI XML refresh done, updating object tree..."));
            updateObjectTree( reply.value("ui_filename").value(0) );

            statusbar(tr("UI XML refresh done, updating behaviours..."));
            /*bool behavioursOk =*/
            updateBehaviourXml();

            statusbar(tr("UI XML refresh done, updating properties..."));
            updatePropertiesTable();

            statusbar(tr("UI XML refresh complete!"), 1000 );
            updateWindowTitle();

        }
        // bitfield: 2 for UI XML refresh
        if (doExlusiveDisconnectAfterRefreshes == 2) {
            disconnectExclusiveSUT();
        }
        doExlusiveDisconnectAfterRefreshes &= ~2;
        objectTree->setDisabled(false);
        propertiesDock->setDisabled(false);
        break;

    case commandRefreshImage:
        if (handleNormally) {
            statusbar(tr("Image refresh done, updating..."), 1000);
            imageWidget->disableDrawHighlight();
            imageWidget->refreshImage( reply.value("image_filename").value(0));
            imageWidget->repaint();
            statusbar(tr("Image refresh complete!"), 1000);
        }
        // bitfield: 1 for image
        if (doExlusiveDisconnectAfterRefreshes == 1) {
            disconnectExclusiveSUT();
        }
        doExlusiveDisconnectAfterRefreshes &= ~1;
        imageViewDock->setDisabled(false);
        break;

    case commandKeyPress:
        break;

    case commandSetAttribute:
        break;

    case commandCheckApiFixture:
        break;

    case commandBehavioursXml:
        break;

    case commandGetVersionNumber:
        break;

    case commandSignalList:
        break;

    case commandGetDeviceParameter:
        break;

    case commandGetAllDeviceParameters:
        break;

    case commandStartApplication:
        break;

    case commandInvalid:
        qDebug() << FCFL << "got message type commandInvalid!";
    }

}


void MainWindow::messageTimeoutSlot()
{
    statusbar(tr("TDriver interface time-out!"), 1000);
    resetMessageSequenceFlags();
}


void MainWindow::resetMessageSequenceFlags()
{
    doRefreshAfterAppList=false;
    doExlusiveDisconnectAfterRefreshes=0;
    if (messageTimeoutTimer) messageTimeoutTimer->stop();
}

void MainWindow::processErrorMessage(ExecuteCommandType commandType, const QString &commandString,
                                     const BAListMap &msg, const QString &additionalInformation,
                                     unsigned &resultEnum,
                                     QString &clearError, QString &shortError, QString &fullError)
{
    QStringList errList;
    foreach(const QByteArray &ba, msg.value("error")) {
        errList << QString(ba);
    }
    QString tdriverError = errList.join("\n");

    QStringList exList;
    foreach(const QByteArray &ba, msg.value("exception")) {
        exList << QString(ba);
    }

    // do clearError
    {
        switch ( commandType ) {
        case commandListApps: clearError = tr("Error retrieving applications list."); break;
        case commandClassMethods: clearError = tr("Error retrieving methods list for %1.").arg(additionalInformation); break;
        case commandSignalList: clearError = tr("Error retrieving signal list for %1.").arg(additionalInformation); break;
        case commandDisconnectSUT: clearError = tr("Error disconnecting SUT %1.").arg(additionalInformation); break;
        case commandTapScreen: clearError = tr("Error performing tap to screen."); break;
        case commandRefreshUI: clearError = tr("Failed to refresh UI data."); break;
        case commandRefreshImage: clearError = tr("Failed to refresh screen capture image."); break;
        case commandKeyPress: clearError = tr("Failed to press key %1.").arg(additionalInformation); break;
        case commandSetAttribute: clearError = tr("Failed to set attribute %1.").arg(additionalInformation); break;
        case commandGetVersionNumber: clearError = tr("Failed to retrieve TDriver version number."); break;
        case commandStartApplication: clearError = tr("Failed to start application."); break;
        default: clearError = tr("Error with command string '%1'").arg(commandString);
        }
    }

    // do shortError
    {
        if ( commandType == commandCheckApiFixture) {
            resultEnum = SILENT;
        }

        else if ( tdriverError.contains( "No plugins and no ui for server" ) ) {
            shortError = tr( "Failed to refresh application screen capture.\n\nLaunch some application with UI and try again." );
            resultEnum = WARNING;
        }

        else if ( tdriverError.contains( "No connection could be made because the target machine actively refused it." ) ) {
            shortError = tr( "Please start/restart QTTAS server." );
            resultEnum = DISCONNECT | RETRY;
        }

        else if( tdriverError.contains( "An existing connection was forcibly closed by the remote host." ) ) {
            shortError = tr( "Please disconnect the SUT from file menu and try again.\n\nIf the problem persists, restart QTTAS server/device or contact support." );
            resultEnum = DISCONNECT | RETRY;
        }

        else if( tdriverError.contains( "Connection refused" ) ) {
            shortError = tr( "Unable to connect to target. Please verify that required servers are running and target is connected properly.\n\nIf the problem persists, contact support." );
            resultEnum = DISCONNECT | RETRY;
        }

        else if( tdriverError.contains( "No data retrieved (IOError)" ) ) {
            shortError = tr( "Unable to read data from target. Please verify that required servers are running and target is connected properly.\n\nIf the problem persists, contact support." );
            resultEnum = DISCONNECT | RETRY;
        }

        else if( tdriverError.contains( "Broken pipe (Errno::EPIPE)" ) ) {
            shortError = tr( "Unable to connect to target due to broken pipe.\n\nPlease disconnect SUT, verify that required servers are running/target is connected properly and try again.\n\nIf the problem persists, contact support." );
            resultEnum = DISCONNECT | RETRY;
        }

        else {
            // unknown error
            shortError = tdriverError;
            resultEnum = FAIL;
        }
    }

    // do fullError
    {
        fullError = tr("%1\n\n%2").arg(clearError).arg(shortError);
        if (shortError != tdriverError) {
            fullError += "\n(original: " + tdriverError + ")";
        }
        if (!exList.empty()) {
            while (exList.size() < 2) exList << QString();
            fullError += tr("\n\nException '%1':\n%2\n\nBacktrace:\n%3").
                    arg(exList.takeFirst()).
                    arg(exList.takeFirst()).
                    arg(exList.join("\n"));
            fullError.replace(":in `", ":\n  in `");
        }
    }
}


bool MainWindow::sendTDriverCommand( ExecuteCommandType commandType,
                                    const QString &commandString,
                                    const QString &errorName)
{
    BAListMap msg;
    msg["input"] = commandString.toAscii().split(' ');

    quint32 seqNum = TDriverRubyInterface::globalInstance()->sendCmd(
                TDriverUtil::visualizationId, msg);
    qDebug() << FCFL << "SENT SEQNUM" << seqNum;

    if (seqNum > 0) {

        sentTDriverMsgs[seqNum] = SentTDriverMsg(commandType, msg, errorName);

        int default_timeout = TDriverUtil::quotedToInt(activeDeviceParams.value("default_timeout"))*1000;
        if (default_timeout <= 0) default_timeout=35000;
        messageTimeoutTimer->start(default_timeout);
        return true;
    }
    else {
        // send message with sequence number 0 to trigger any followup action to happen
        if (!errorName.isNull()) {
            statusbar(tr("ERROR: Sending %1 command to TDriver failed!").arg(errorName), 1000);
        }
        sentTDriverMsgs[0] = SentTDriverMsg(commandType, msg, errorName, -1);
        receiveTDriverMessage(0, TDriverUtil::visualizationId);
        return false;
    }
}


bool MainWindow::resendTDriverCommand(SentTDriverMsg &msg)
{
    msg.resends++;

    quint32 seqNum = TDriverRubyInterface::globalInstance()->sendCmd(
                TDriverUtil::visualizationId, msg.msg);
    qDebug() << FCFL << "RESENT SEQNUM" << seqNum;
    if (seqNum > 0) {
        sentTDriverMsgs[seqNum] = SentTDriverMsg(msg);
        int default_timeout = TDriverUtil::quotedToInt(activeDeviceParams.value("default_timeout"))*1000;
        if (default_timeout <= 0) default_timeout=35000;
        messageTimeoutTimer->start(default_timeout);
        return true;
    }
    else {
        if (!msg.err.isNull()) {
            statusbar(tr("ERROR: Sending %1 command to TDriver failed!").arg(msg.err), 1000);
        }
        sentTDriverMsgs[0] = msg;
        receiveTDriverMessage(0, TDriverUtil::visualizationId);
        return false;
    }
}


bool MainWindow::executeTDriverCommand( ExecuteCommandType commandType,
                                       const QString &commandString,
                                       const QString &additionalInformation,
                                       BAListMap *reply )
{
    QString clearError;
    QString shortError;
    QString fullError;
    bool exit = false;
    bool result = true;
    int iteration = 0;
    int default_timeout = TDriverUtil::quotedToInt(activeDeviceParams.value("default_timeout"))*1000;
    unsigned resultEnum = OK;

    if (default_timeout <= 0){
        default_timeout=35000;
    }

    do {
        BAListMap msg;
        msg["input"] = commandString.toAscii().split(' ');

        qDebug() << FCFL << "going to execute" << msg << "Using timeout: " << default_timeout;
        QTime t;
        t.start();
        /*bool response1 =*/
        TDriverRubyInterface::globalInstance()->executeCmd(TDriverUtil::visualizationId, msg, default_timeout );
        if (msg.contains("error")) {
            qDebug() << FCFL << "failure time" << float(t.elapsed())/1000.0 << "reply" << msg;

            result = false;
            exit = true;
            processErrorMessage(commandType, commandString, msg, additionalInformation,
                                resultEnum, clearError, shortError, fullError);

            if ( resultEnum & FAIL || iteration > 0 ) {
                // exit if failed again or no retries allowed..
                exit = true;
            }

            if ( resultEnum & DISCONNECT ) {
                // disconnect
                msg.clear();
                msg["input"] << activeDevice.toAscii() << "disconnect";
                /*bool response2 =*/
                TDriverRubyInterface::globalInstance()->executeCmd(TDriverUtil::visualizationId, msg, default_timeout );
                if (msg.contains("error")) {
                    fullError += "\n\nDisconnect after error failed!";
                    result = false;
                    exit = true;
                }
                else {
                    fullError += "\n\nDisconnect after error succeeded.";
                    // disconnect passed -- retry
                    currentApplication.clear();
                    result = false;
                    if (!(resultEnum & FAIL)) exit = false;
                }
            }
        }
        else {
            qDebug() << FCFL << "success time" << float(t.elapsed())/1000.0 << "reply keys" << msg.keys();
            if (reply) {
                *reply = msg;
            }
            result = true;
            exit = true;
        }

        iteration++;
    } while (!exit);

    if ( !result && !(resultEnum & SILENT) ) {
        tdriverMsgAppend(fullError);
    }

    return result;
}


void MainWindow::createActions()
{

    parseSUT = new QAction( tr("&Parse TDriver parameters xml file..." ), this);
    parseSUT->setObjectName("main parsesut");
    parseSUT->setShortcuts(QList<QKeySequence>() <<
                           QKeySequence(tr("Ctrl+M, P")) <<
                           QKeySequence(tr("Ctrl+M, Ctrl+P")));
    parseSUT->setDisabled( true );

    connect( parseSUT, SIGNAL( triggered() ), this, SLOT(getParameterXML()));

    loadXmlAction = new QAction( this );
    loadXmlAction->setObjectName("main loadxml");
    loadXmlAction->setText( "&Load state XML file" );
    loadXmlAction->setShortcuts(QList<QKeySequence>() <<
                                QKeySequence(tr("Ctrl+M, L")) <<
                                QKeySequence(tr("Ctrl+M, Ctrl+L")));

    connect( loadXmlAction, SIGNAL( triggered() ), this, SLOT(loadFileData()));


    saveStateAction = new QAction( this );
    saveStateAction->setObjectName("main savestate");
    saveStateAction->setText( "&Save current state to folder..." );
    saveStateAction->setShortcuts(QList<QKeySequence>() <<
                                  QKeySequence(tr("Ctrl+M, S")) <<
                                  QKeySequence(tr("Ctrl+M, Ctrl+S")));

    connect( saveStateAction, SIGNAL( triggered() ), this, SLOT(saveStateAsArchive()));

    fontAction = new QAction(tr( "Select default f&ont..." ), this);
    fontAction->setObjectName("main font");
    //fontAction->setShortcut( QKeySequence( Qt::ControlModifier + Qt::Key_T ) );

    connect( fontAction, SIGNAL( triggered() ), this, SLOT(openFontDialog()));

    refreshAction = new QAction(tr("&Refresh"), this);
    refreshAction->setObjectName("main refresh");
    refreshAction->setShortcut(QKeySequence(tr("Ctrl+R")));
    // note: QKeySequence(QKeySequence::Refresh) is F5 in some platforms, Ctrl+R in others
    //refreshAction->setDisabled( true );

    connect( refreshAction, SIGNAL( triggered() ), this, SLOT(forceRefreshData()));

    delayedRefreshAction = new QAction(tr("Refresh in 5 secs"), this );
    delayedRefreshAction->setObjectName("main delayed delayedRefresh");
    delayedRefreshAction->setShortcut(QKeySequence(tr("Ctrl+Alt+R")));
    //delayedRefreshAction->setDisabled( true );

    connect( delayedRefreshAction, SIGNAL(triggered()), this, SLOT(delayedRefreshData()));

    disconnectCurrentSUT = new QAction( tr( "Dis&connect SUT" ), this );
    disconnectCurrentSUT->setObjectName("main disconnectsut");
    disconnectCurrentSUT->setShortcuts(QList<QKeySequence>() <<
                                       QKeySequence(tr("Ctrl+M, C")) <<
                                       QKeySequence(tr("Ctrl+M, Ctrl+C")));
    disconnectCurrentSUT->setDisabled( true );

    connect( disconnectCurrentSUT, SIGNAL( triggered() ), this, SLOT(disconnectSUT()));

    exitAction = new QAction(tr("E&xit"), this);
    exitAction->setObjectName("main exit");
    exitAction->setShortcut( QKeySequence( tr("Alt+X")));

    connect( exitAction, SIGNAL( triggered() ), this, SLOT(close()));

    findAction = new QAction(tr("&Find"), this);
    findAction->setObjectName("main find");
    //findAction->setShortcut( QKeySequence(tr("Ctrl+F")));

    connect( findAction, SIGNAL( triggered() ), this, SLOT( showFindDialog() ) );

    startAppAction = new QAction(tr("Start New Application"), this);
    startAppAction->setObjectName("main startapp");

    connect( startAppAction, SIGNAL( triggered() ), this, SLOT( showStartAppDialog() ) );

    viewExpandAll = new QAction( this );
    viewExpandAll->setObjectName("main tree expand");
    viewExpandAll->setText( "&Expand all" );
    viewExpandAll->setShortcut( QKeySequence( Qt::ControlModifier + Qt::Key_Right ) );
    viewExpandAll->setCheckable( false );

    connect( viewExpandAll, SIGNAL( triggered() ), this, SLOT( objectTreeExpandAll() ) );

    viewCollapseAll = new QAction( this );
    viewCollapseAll->setObjectName("main tree collapse");
    viewCollapseAll->setText( "&Collapse all" );
    viewCollapseAll->setShortcut( QKeySequence( Qt::ControlModifier + Qt::Key_Left ) );
    viewCollapseAll->setCheckable( false );

    connect( viewCollapseAll, SIGNAL( triggered() ), this, SLOT( objectTreeCollapseAll() ) );

    showXmlAction = new QAction( this );
    showXmlAction->setObjectName("main showxml");
    showXmlAction->setText( tr( "&Show source XML" ) );
    showXmlAction->setShortcut( QKeySequence( Qt::ControlModifier + Qt::Key_U ) );

    connect( showXmlAction, SIGNAL( triggered() ), this, SLOT( showXMLDialog() ) );

    recordAction = new QAction( this );
    recordAction->setObjectName("main record");
    recordAction->setText( tr( "&Open Recorder Dialog" ) );
    recordAction->setShortcut( tr( "F6" ) );

    connect( recordAction, SIGNAL( triggered() ), this, SLOT( openRecordWindow() ) );

    visualizerAssistant = new QAction( this );
    visualizerAssistant->setObjectName("main help assistant");
    visualizerAssistant->setText( tr( "TDriver &Help" ) );
    // Remove the next line if F1 presses are to be handled in the custom event handler ( for context sensitivity ).
    visualizerAssistant->setShortcut( tr( "F1" ) );

    connect( visualizerAssistant, SIGNAL( triggered() ), this, SLOT( showMainVisualizerAssistant() ) );

    visualizerHelp = new QAction( this );
    visualizerHelp->setObjectName("main help visualizer");
    visualizerHelp->setText( tr( "&Visualizer Help" ) );

    connect( visualizerHelp, SIGNAL( triggered() ), this, SLOT( showVisualizerHelp() ) );

    aboutVisualizer = new QAction(this);
    aboutVisualizer->setObjectName("main help about");
    aboutVisualizer->setText( tr( "About Visualizer" ));

    connect( aboutVisualizer, SIGNAL( triggered() ), this, SLOT( showAboutVisualizer() ) );

}


void MainWindow::createAppsBar()
{
    appsBar = new QToolBar( tr( "Applications" ));
    appsBar->setObjectName("apps");
    //appsBar->addAction(exitAction);

    addToolBar(Qt::LeftToolBarArea, appsBar);
}


void MainWindow::createShortcutsBar()
{
    shortcutsBar = new QToolBar( tr( "Shortcuts" ));
    shortcutsBar->setObjectName("shortcuts");

    shortcutsBar->addAction(refreshAction);
    shortcutsBar->addSeparator();
    shortcutsBar->addAction(delayedRefreshAction);
    shortcutsBar->addSeparator();
    shortcutsBar->addAction(showXmlAction);
    shortcutsBar->addSeparator();
    shortcutsBar->addAction(loadXmlAction);

    //shortcutsBar->addAction(exitAction);

    addToolBar(shortcutsBar);
}

void MainWindow::createClipboardBar()
{

    clipboardBar = new QToolBar( tr("Clipboard contents"));
    clipboardBar->setObjectName("clipboard");

    //clipboardBar->setFloating( false );
    clipboardBar->setVisible( true );

    objectAttributeLineEdit = new QLineEdit;
    objectAttributeLineEdit->setObjectName("clipboard");

    clipboardBar->addWidget( new QLabel(tr("Clipboard: ")) );
    clipboardBar->addWidget( objectAttributeLineEdit );

    addToolBar(Qt::TopToolBarArea, clipboardBar);
}

void MainWindow::updateClipboardText( QString text, bool appendText ) {

    QClipboard *clipboard = QApplication::clipboard();

    if ( appendText ) { text.prepend( clipboard->text().append(".") ); }

    clipboard->setText( text );
    objectAttributeLineEdit->setText( text );

}
