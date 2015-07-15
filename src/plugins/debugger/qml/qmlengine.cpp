/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company.  For licensing terms and
** conditions see http://www.qt.io/terms-conditions.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, The Qt Company gives you certain additional
** rights.  These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "qmlengine.h"

#include "interactiveinterpreter.h"
#include "qmlinspectoradapter.h"
#include "qmlinspectoragent.h"
#include "qmlv8debuggerclientconstants.h"
#include "qmlengineutils.h"

#include <debugger/breakhandler.h>
#include <debugger/debuggeractions.h>
#include <debugger/debuggercore.h>
#include <debugger/debuggerinternalconstants.h>
#include <debugger/debuggerruncontrol.h>
#include <debugger/debuggerstringutils.h>
#include <debugger/debuggertooltipmanager.h>
#include <debugger/sourcefileshandler.h>
#include <debugger/stackhandler.h>
#include <debugger/threaddata.h>
#include <debugger/watchhandler.h>
#include <debugger/watchwindow.h>

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/helpmanager.h>
#include <coreplugin/icore.h>

#include <projectexplorer/applicationlauncher.h>

#include <qmljseditor/qmljseditorconstants.h>
#include <qmljs/qmljsmodelmanagerinterface.h>
#include <qmljs/consolemanagerinterface.h>

#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>

#include <utils/qtcassert.h>

#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTimer>

#define DEBUG_QML 0
#if DEBUG_QML
#   define SDEBUG(s) qDebug() << s
#else
#   define SDEBUG(s)
#endif
# define XSDEBUG(s) qDebug() << s

using namespace Core;
using namespace ProjectExplorer;
using namespace QmlDebug;
using namespace QmlJS;
using namespace TextEditor;

namespace Debugger {
namespace Internal {

enum Exceptions
{
    NoExceptions,
    UncaughtExceptions,
    AllExceptions
};

enum StepAction
{
    Continue,
    StepIn,
    StepOut,
    Next
};

struct QmlV8ObjectData
{
    int handle;
    QByteArray name;
    QByteArray type;
    QVariant value;
    QVariantList properties;
};

class QmlEnginePrivate : QmlDebugClient
{
public:
    QmlEnginePrivate(QmlEngine *engine_, QmlDebugConnection *connection_)
        : QmlDebugClient(QLatin1String("V8Debugger"), connection_),
          engine(engine_),
          inspectorAdapter(engine, connection_),
          connection(connection_)
    {}

    void sendMessage(const QByteArray &msg);
    void messageReceived(const QByteArray &data);
    void stateChanged(State state);

    void connect();
    void disconnect();

    void continueDebugging(StepAction stepAction);

    void evaluate(const QString expr, bool global = false, bool disableBreak = false,
                  int frame = -1, bool addContext = false);
    void lookup(const QList<int> handles, bool includeSource = false);
    void backtrace(int fromFrame = -1, int toFrame = -1, bool bottom = false);
    void frame(int number = -1);
    void scope(int number = -1, int frameNumber = -1);
    void scripts(int types = 4, const QList<int> ids = QList<int>(),
                 bool includeSource = false, const QVariant filter = QVariant());

    void setBreakpoint(const QString type, const QString target,
                       bool enabled = true,int line = 0, int column = 0,
                       const QString condition = QString(), int ignoreCount = -1);
    void clearBreakpoint(int breakpoint);
    void setExceptionBreak(Exceptions type, bool enabled = false);

    void version();
    void clearCache();

    void sendAndLogV8Request(const QJsonObject &request);

    QByteArray packMessage(const QByteArray &type, const QByteArray &message = QByteArray());
    QJsonObject initObject();

    void expandObject(const QByteArray &iname, quint64 objectId);
    void flushSendBuffer();

    void handleBacktrace(const QVariant &bodyVal, const QVariant &refsVal);
    void handleLookup(const QVariant &bodyVal, const QVariant &refsVal);
    void handleEvaluate(int sequence, bool success, const QVariant &bodyVal, const QVariant &refsVal);
    void handleFrame(const QVariant &bodyVal, const QVariant &refsVal);
    void handleScope(const QVariant &bodyVal, const QVariant &refsVal);
    StackFrame extractStackFrame(const QVariant &bodyVal, const QVariant &refsVal);

    bool canEvaluateScript(const QString &script);
    void updateScriptSource(const QString &fileName, int lineOffset, int columnOffset, const QString &source);

public:
    int sequence = -1;
    QmlEngine *engine;
    QHash<BreakpointModelId, int> breakpoints;
    QHash<int, BreakpointModelId> breakpointsSync;
    QList<int> breakpointsTemp;

    QHash<int, QString> evaluatingExpression;
    QHash<int, QByteArray> localsAndWatchers;
    QList<int> updateLocalsAndWatchers;
    QList<int> debuggerCommands;

    //Cache
    QList<int> currentFrameScopes;
    QHash<int, int> stackIndexLookup;

    StepAction previousStepAction = Continue;

    QList<QByteArray> sendBuffer;

    QHash<QString, QTextDocument*> sourceDocuments;
    QHash<QString, QWeakPointer<BaseTextEditor> > sourceEditors;
    InteractiveInterpreter interpreter;
    ApplicationLauncher applicationLauncher;
    QmlInspectorAdapter inspectorAdapter;
    QmlOutputParser outputParser;

    QTimer noDebugOutputTimer;
    QHash<QString,Breakpoint> pendingBreakpoints;
    QList<quint32> queryIds;
    bool retryOnConnectFail = false;
    bool automaticConnect = false;

    QTimer connectionTimer;
    QmlDebug::QmlDebugConnection *connection;
    QmlDebug::QDebugMessageClient *msgClient = 0;
};

static void updateDocument(IDocument *document, const QTextDocument *textDocument)
{
    if (auto baseTextDocument = qobject_cast<TextDocument *>(document))
        baseTextDocument->document()->setPlainText(textDocument->toPlainText());
}


///////////////////////////////////////////////////////////////////////
//
// QmlEngine
//
///////////////////////////////////////////////////////////////////////

QmlEngine::QmlEngine(const DebuggerRunParameters &startParameters, DebuggerEngine *masterEngine)
  : DebuggerEngine(startParameters),
    d(new QmlEnginePrivate(this, new QmlDebugConnection(this)))
{
    setObjectName(QLatin1String("QmlEngine"));

    if (masterEngine)
        setMasterEngine(masterEngine);

    connect(stackHandler(), &StackHandler::stackChanged,
            this, &QmlEngine::updateCurrentContext);
    connect(stackHandler(), &StackHandler::currentIndexChanged,
            this, &QmlEngine::updateCurrentContext);
    connect(inspectorView(), SIGNAL(currentIndexChanged(QModelIndex)),
            SLOT(updateCurrentContext()));
    connect(d->inspectorAdapter.agent(), &QmlInspectorAgent::expressionResult,
            this, &QmlEngine::expressionEvaluated);

    connect(&d->applicationLauncher, &ApplicationLauncher::processExited,
            this, &QmlEngine::disconnected);
    connect(&d->applicationLauncher, &ApplicationLauncher::appendMessage,
            this, &QmlEngine::appendMessage);
    connect(&d->applicationLauncher, &ApplicationLauncher::processStarted,
            &d->noDebugOutputTimer, static_cast<void(QTimer::*)()>(&QTimer::start));

    d->outputParser.setNoOutputText(ApplicationLauncher::msgWinCannotRetrieveDebuggingOutput());
    connect(&d->outputParser, &QmlOutputParser::waitingForConnectionOnPort,
            this, &QmlEngine::beginConnection);
    connect(&d->outputParser, &QmlOutputParser::noOutputMessage,
            this, [this] { tryToConnect(); });
    connect(&d->outputParser, &QmlOutputParser::errorMessage,
            this, &QmlEngine::appStartupFailed);

    // Only wait 8 seconds for the 'Waiting for connection' on application output,
    // then just try to connect (application output might be redirected / blocked)
    d->noDebugOutputTimer.setSingleShot(true);
    d->noDebugOutputTimer.setInterval(8000);
    connect(&d->noDebugOutputTimer, SIGNAL(timeout()), this, SLOT(tryToConnect()));

    if (auto mmIface = ModelManagerInterface::instance()) {
        connect(mmIface, &ModelManagerInterface::documentUpdated,
                this, &QmlEngine::documentUpdated);
    }
    // we won't get any debug output
    if (startParameters.useTerminal) {
        d->noDebugOutputTimer.setInterval(0);
        d->retryOnConnectFail = true;
        d->automaticConnect = true;
    }

    if (auto consoleManager = ConsoleManagerInterface::instance())
        consoleManager->setScriptEvaluator(this);


    d->connectionTimer.setInterval(4000);
    d->connectionTimer.setSingleShot(true);
    connect(&d->connectionTimer, &QTimer::timeout,
            this, &QmlEngine::checkConnectionState);

    connect(d->connection, &QmlDebugConnection::stateMessage,
            this, &QmlEngine::showConnectionStateMessage);
    connect(d->connection, &QmlDebugConnection::errorMessage,
            this, &QmlEngine::showConnectionErrorMessage);
    connect(d->connection, &QmlDebugConnection::error,
            this, &QmlEngine::connectionErrorOccurred);
    connect(d->connection, &QmlDebugConnection::opened,
            &d->connectionTimer, &QTimer::stop);
    connect(d->connection, &QmlDebugConnection::opened,
            this, &QmlEngine::connectionEstablished);
    connect(d->connection, &QmlDebugConnection::closed,
            this, &QmlEngine::disconnected);

    d->msgClient = new QDebugMessageClient(d->connection);
    connect(d->msgClient, &QDebugMessageClient::newState,
            this, &QmlEngine::clientStateChanged);
    connect(d->msgClient, &QDebugMessageClient::message,
            this, &appendDebugOutput);
}

QmlEngine::~QmlEngine()
{
    QSet<IDocument *> documentsToClose;

    QHash<QString, QWeakPointer<BaseTextEditor> >::iterator iter;
    for (iter = d->sourceEditors.begin(); iter != d->sourceEditors.end(); ++iter) {
        QWeakPointer<BaseTextEditor> textEditPtr = iter.value();
        if (textEditPtr)
            documentsToClose << textEditPtr.data()->document();
    }
    EditorManager::closeDocuments(documentsToClose.toList());

    delete d;
}

void QmlEngine::setupInferior()
{
    QTC_ASSERT(state() == InferiorSetupRequested, qDebug() << state());

    notifyInferiorSetupOk();

    if (d->automaticConnect)
        beginConnection();
}

void QmlEngine::appendMessage(const QString &msg, Utils::OutputFormat /* format */)
{
    showMessage(msg, AppOutput); // FIXME: Redirect to RunControl
}

void QmlEngine::connectionEstablished()
{
    attemptBreakpointSynchronization();

    if (!watchHandler()->watcherNames().isEmpty())
        synchronizeWatchers();
    connect(watchModel(), &QAbstractItemModel::layoutChanged,
            this, &QmlEngine::synchronizeWatchers);

    if (state() == EngineRunRequested)
        notifyEngineRunAndInferiorRunOk();
}

void QmlEngine::tryToConnect(quint16 port)
{
    showMessage(QLatin1String("QML Debugger: No application output received in time, trying to connect ..."), LogStatus);
    d->retryOnConnectFail = true;
    if (state() == EngineRunRequested) {
        if (isSlaveEngine()) {
            // Probably cpp is being debugged and hence we did not get the output yet.
            if (!masterEngine()->isDying()) {
                d->noDebugOutputTimer.setInterval(4000);
                d->noDebugOutputTimer.start();
            }
            else
                appStartupFailed(tr("No application output received in time"));
        } else {
            beginConnection(port);
        }
    } else {
        d->automaticConnect = true;
    }
}

void QmlEngine::beginConnection(quint16 port)
{
    d->noDebugOutputTimer.stop();

    if (state() != EngineRunRequested && d->retryOnConnectFail)
        return;

    QTC_ASSERT(state() == EngineRunRequested, return);

    QString host =  runParameters().qmlServerAddress;
    // Use localhost as default
    if (host.isEmpty())
        host = QLatin1String("localhost");

    /*
     * Let plugin-specific code override the port printed by the application. This is necessary
     * in the case of port forwarding, when the port the application listens on is not the same that
     * we want to connect to.
     * NOTE: It is still necessary to wait for the output in that case, because otherwise we cannot
     * be sure that the port is already open. The usual method of trying to connect repeatedly
     * will not work, because the intermediate port is already open. So the connection
     * will be accepted on that port but the forwarding to the target port will fail and
     * the connection will be closed again (instead of returning the "connection refused"
     * error that we expect).
     */
    if (runParameters().qmlServerPort > 0)
        port = runParameters().qmlServerPort;

    if (!d->connection || d->connection->isOpen())
        return;

    d->connection->connectToHost(host, port);

    //A timeout to check the connection state
    d->connectionTimer.start();
}

void QmlEngine::connectionStartupFailed()
{
    if (d->retryOnConnectFail) {
        // retry after 3 seconds ...
        QTimer::singleShot(3000, this, SLOT(beginConnection()));
        return;
    }

    QMessageBox *infoBox = new QMessageBox(ICore::mainWindow());
    infoBox->setIcon(QMessageBox::Critical);
    infoBox->setWindowTitle(tr("Qt Creator"));
    infoBox->setText(tr("Could not connect to the in-process QML debugger."
                        "\nDo you want to retry?"));
    infoBox->setStandardButtons(QMessageBox::Retry | QMessageBox::Cancel |
                                QMessageBox::Help);
    infoBox->setDefaultButton(QMessageBox::Retry);
    infoBox->setModal(true);

    connect(infoBox, &QDialog::finished,
            this, &QmlEngine::errorMessageBoxFinished);

    infoBox->show();
}

void QmlEngine::appStartupFailed(const QString &errorMessage)
{
    QString error = tr("Could not connect to the in-process QML debugger."
                       "\n%1").arg(errorMessage);

    if (isMasterEngine()) {
        QMessageBox *infoBox = new QMessageBox(ICore::mainWindow());
        infoBox->setIcon(QMessageBox::Critical);
        infoBox->setWindowTitle(tr("Qt Creator"));
        infoBox->setText(error);
        infoBox->setStandardButtons(QMessageBox::Ok | QMessageBox::Help);
        infoBox->setDefaultButton(QMessageBox::Ok);
        connect(infoBox, &QDialog::finished,
                this, &QmlEngine::errorMessageBoxFinished);
        infoBox->show();
    } else {
        showMessage(error, StatusBar);
    }

    notifyEngineRunFailed();
}

void QmlEngine::errorMessageBoxFinished(int result)
{
    switch (result) {
    case QMessageBox::Retry: {
        beginConnection();
        break;
    }
    case QMessageBox::Help: {
        HelpManager::handleHelpRequest(QLatin1String("qthelp://org.qt-project.qtcreator/doc/creator-debugging-qml.html"));
        // fall through
    }
    default:
        if (state() == InferiorRunOk) {
            notifyInferiorSpontaneousStop();
            notifyInferiorIll();
        } else if (state() == EngineRunRequested) {
            notifyEngineRunFailed();
        }
        break;
    }
}

void QmlEngine::filterApplicationMessage(const QString &output, int /*channel*/) const
{
    d->outputParser.processOutput(output);
}

void QmlEngine::showMessage(const QString &msg, int channel, int timeout) const
{
    if (channel == AppOutput || channel == AppError)
        filterApplicationMessage(msg, channel);
    DebuggerEngine::showMessage(msg, channel, timeout);
}

void QmlEngine::gotoLocation(const Location &location)
{
    const QString fileName = location.fileName();
    if (QUrl(fileName).isLocalFile()) {
        // internal file from source files -> show generated .js
        QTC_ASSERT(d->sourceDocuments.contains(fileName), return);

        QString titlePattern = tr("JS Source for %1").arg(fileName);
        //Check if there are open documents with the same title
        foreach (IDocument *document, DocumentModel::openedDocuments()) {
            if (document->displayName() == titlePattern) {
                EditorManager::activateEditorForDocument(document);
                return;
            }
        }
        IEditor *editor = EditorManager::openEditorWithContents(
                    QmlJSEditor::Constants::C_QMLJSEDITOR_ID, &titlePattern);
        if (editor) {
            editor->document()->setProperty(Constants::OPENED_BY_DEBUGGER, true);
            if (auto plainTextEdit = qobject_cast<QPlainTextEdit *>(editor->widget()))
                plainTextEdit->setReadOnly(true);
            updateDocument(editor->document(), d->sourceDocuments.value(fileName));
        }
    } else {
        DebuggerEngine::gotoLocation(location);
    }
}

void QmlEngine::closeConnection()
{
    disconnect(watchModel(), &QAbstractItemModel::layoutChanged,
               this, &QmlEngine::synchronizeWatchers);

    if (d->connectionTimer.isActive()) {
        d->connectionTimer.stop();
    } else {
        if (d->connection)
            d->connection->close();
    }
}

void QmlEngine::runEngine()
{
    QTC_ASSERT(state() == EngineRunRequested, qDebug() << state());

    if (!isSlaveEngine()) {
        if (runParameters().startMode == AttachToRemoteServer)
            d->noDebugOutputTimer.start();
        else if (runParameters().startMode == AttachToRemoteProcess)
            beginConnection();
        else
            startApplicationLauncher();
    } else {
        d->noDebugOutputTimer.start();
    }
}

void QmlEngine::startApplicationLauncher()
{
    if (!d->applicationLauncher.isRunning()) {
        appendMessage(tr("Starting %1 %2").arg(
                          QDir::toNativeSeparators(runParameters().executable),
                          runParameters().processArgs)
                      + QLatin1Char('\n')
                     , Utils::NormalMessageFormat);
        d->applicationLauncher.start(ApplicationLauncher::Gui,
                                    runParameters().executable,
                                    runParameters().processArgs);
    }
}

void QmlEngine::stopApplicationLauncher()
{
    if (d->applicationLauncher.isRunning()) {
        disconnect(&d->applicationLauncher, &ApplicationLauncher::processExited,
                   this, &QmlEngine::disconnected);
        d->applicationLauncher.stop();
    }
}

void QmlEngine::notifyEngineRemoteSetupFinished(const RemoteSetupResult &result)
{
    DebuggerEngine::notifyEngineRemoteSetupFinished(result);

    if (result.success) {
        if (result.qmlServerPort != InvalidPort)
            runParameters().qmlServerPort = result.qmlServerPort;

        notifyEngineSetupOk();

        // The remote setup can take while especialy with mixed debugging.
        // Just waiting for 8 seconds is not enough. Increase the timeout
        // to 60 s
        // In case we get an output the d->outputParser will start the connection.
        d->noDebugOutputTimer.setInterval(60000);
    }
    else {
        if (isMasterEngine())
            QMessageBox::critical(ICore::dialogParent(), tr("Failed to start application"),
                                  tr("Application startup failed: %1").arg(result.reason));
        notifyEngineSetupFailed();
    }
}

void QmlEngine::notifyEngineRemoteServerRunning(const QByteArray &serverChannel, int pid)
{
    bool ok = false;
    quint16 qmlPort = serverChannel.toUInt(&ok);
    if (ok)
        runParameters().qmlServerPort = qmlPort;
    else
        qWarning() << tr("QML debugging port not set: Unable to convert %1 to unsigned int.").arg(QString::fromLatin1(serverChannel));

    DebuggerEngine::notifyEngineRemoteServerRunning(serverChannel, pid);
    notifyEngineSetupOk();

    // The remote setup can take a while especially with mixed debugging.
    // Just waiting for 8 seconds is not enough. Increase the timeout to 60 s.
    // In case we get an output the d->outputParser will start the connection.
    d->noDebugOutputTimer.setInterval(60000);
}

void QmlEngine::shutdownInferior()
{
    // End session.
    d->disconnect();

    if (isSlaveEngine())
        resetLocation();
    stopApplicationLauncher();
    closeConnection();

    notifyInferiorShutdownOk();
}

void QmlEngine::shutdownEngine()
{
    clearExceptionSelection();

    if (auto consoleManager = ConsoleManagerInterface::instance())
        consoleManager->setScriptEvaluator(0);
    d->noDebugOutputTimer.stop();

   // double check (ill engine?):
    stopApplicationLauncher();

    notifyEngineShutdownOk();
    if (!isSlaveEngine())
        showMessage(QString(), StatusBar);
}

void QmlEngine::setupEngine()
{
    if (runParameters().remoteSetupNeeded) {
        // we need to get the port first
        notifyEngineRequestRemoteSetup();
    } else {
        d->applicationLauncher.setEnvironment(runParameters().environment);
        d->applicationLauncher.setWorkingDirectory(runParameters().workingDirectory);

        // We can't do this in the constructore because runControl() isn't yet defined
        connect(&d->applicationLauncher, &ApplicationLauncher::bringToForegroundRequested,
                runControl(), &RunControl::bringApplicationToForeground,
                Qt::UniqueConnection);

        notifyEngineSetupOk();
    }
}

void QmlEngine::continueInferior()
{
    QTC_ASSERT(state() == InferiorStopOk, qDebug() << state());
    clearExceptionSelection();
    d->continueDebugging(Continue);
    resetLocation();
    notifyInferiorRunRequested();
    notifyInferiorRunOk();
}

void QmlEngine::interruptInferior()
{
    showMessage(_(INTERRUPT), LogInput);
    d->sendMessage(d->packMessage(INTERRUPT));
    notifyInferiorStopOk();
}

void QmlEngine::executeStep()
{
    clearExceptionSelection();
    d->continueDebugging(StepIn);
    notifyInferiorRunRequested();
    notifyInferiorRunOk();
}

void QmlEngine::executeStepI()
{
    clearExceptionSelection();
    d->continueDebugging(StepIn);
    notifyInferiorRunRequested();
    notifyInferiorRunOk();
}

void QmlEngine::executeStepOut()
{
    clearExceptionSelection();
    d->continueDebugging(StepOut);
    notifyInferiorRunRequested();
    notifyInferiorRunOk();
}

void QmlEngine::executeNext()
{
    clearExceptionSelection();
    d->continueDebugging(Next);
    notifyInferiorRunRequested();
    notifyInferiorRunOk();
}

void QmlEngine::executeNextI()
{
    executeNext();
}

void QmlEngine::executeRunToLine(const ContextData &data)
{
    QTC_ASSERT(state() == InferiorStopOk, qDebug() << state());
    showStatusMessage(tr("Run to line %1 (%2) requested...").arg(data.lineNumber).arg(data.fileName), 5000);
    resetLocation();
    ContextData modifiedData = data;
    quint32 line = data.lineNumber;
    quint32 column;
    bool valid;
    if (adjustBreakpointLineAndColumn(data.fileName, &line, &column, &valid))
        modifiedData.lineNumber = line;
    d->setBreakpoint(QString(_(SCRIPTREGEXP)), modifiedData.fileName,
                     true, modifiedData.lineNumber);
    clearExceptionSelection();
    d->continueDebugging(Continue);

    notifyInferiorRunRequested();
    notifyInferiorRunOk();
}

void QmlEngine::executeRunToFunction(const QString &functionName)
{
    Q_UNUSED(functionName)
    XSDEBUG("FIXME:  QmlEngine::executeRunToFunction()");
}

void QmlEngine::executeJumpToLine(const ContextData &data)
{
    Q_UNUSED(data)
    XSDEBUG("FIXME:  QmlEngine::executeJumpToLine()");
}

void QmlEngine::activateFrame(int index)
{
    if (state() != InferiorStopOk && state() != InferiorUnrunnable)
        return;

    if (index != stackHandler()->currentIndex())
        d->frame(d->stackIndexLookup.value(index));

    stackHandler()->setCurrentIndex(index);
    gotoLocation(stackHandler()->frames().value(index));
}

void QmlEngine::selectThread(ThreadId threadId)
{
    Q_UNUSED(threadId)
}

void QmlEngine::insertBreakpoint(Breakpoint bp)
{
    BreakpointState state = bp.state();
    QTC_ASSERT(state == BreakpointInsertRequested, qDebug() << bp << this << state);
    bp.notifyBreakpointInsertProceeding();

    const BreakpointParameters &params = bp.parameters();
    quint32 line = params.lineNumber;
    quint32 column = 0;
    if (params.type == BreakpointByFileAndLine) {
        bool valid = false;
        if (!adjustBreakpointLineAndColumn(params.fileName, &line, &column,
                                           &valid)) {
            d->pendingBreakpoints.insertMulti(params.fileName, bp);
            return;
        }
        if (!valid)
            return;
    }

    if (params.type == BreakpointAtJavaScriptThrow) {
        bp.notifyBreakpointInsertOk();
        d->setExceptionBreak(AllExceptions, params.enabled);

    } else if (params.type == BreakpointByFileAndLine) {
        d->setBreakpoint(QString(_(SCRIPTREGEXP)), params.fileName,
                         params.enabled, line, column,
                         QLatin1String(params.condition), params.ignoreCount);

    } else if (params.type == BreakpointOnQmlSignalEmit) {
        d->setBreakpoint(QString(_(EVENT)), params.functionName, params.enabled);
        bp.notifyBreakpointInsertOk();
    }

    d->breakpointsSync.insert(d->sequence, bp.id());
}

void QmlEngine::removeBreakpoint(Breakpoint bp)
{
    const BreakpointParameters &params = bp.parameters();
    if (params.type == BreakpointByFileAndLine &&
            d->pendingBreakpoints.contains(params.fileName)) {
        auto it = d->pendingBreakpoints.find(params.fileName);
        while (it != d->pendingBreakpoints.end() && it.key() == params.fileName) {
            if (it.value() == bp.id()) {
                d->pendingBreakpoints.erase(it);
                return;
            }
            ++it;
        }
    }

    BreakpointState state = bp.state();
    QTC_ASSERT(state == BreakpointRemoveRequested, qDebug() << bp << this << state);
    bp.notifyBreakpointRemoveProceeding();

    int breakpoint = d->breakpoints.value(bp.id());
    d->breakpoints.remove(bp.id());

    if (params.type == BreakpointAtJavaScriptThrow)
        d->setExceptionBreak(AllExceptions);
    else if (params.type == BreakpointOnQmlSignalEmit)
        d->setBreakpoint(QString(_(EVENT)), params.functionName, false);
    else
        d->clearBreakpoint(breakpoint);

    if (bp.state() == BreakpointRemoveProceeding)
        bp.notifyBreakpointRemoveOk();
}

void QmlEngine::changeBreakpoint(Breakpoint bp)
{
    BreakpointState state = bp.state();
    QTC_ASSERT(state == BreakpointChangeRequested, qDebug() << bp << this << state);
    bp.notifyBreakpointChangeProceeding();

    const BreakpointParameters &params = bp.parameters();

    BreakpointResponse br = bp.response();
    if (params.type == BreakpointAtJavaScriptThrow) {
        d->setExceptionBreak(AllExceptions, params.enabled);
        br.enabled = params.enabled;
        bp.setResponse(br);
    } else if (params.type == BreakpointOnQmlSignalEmit) {
        d->setBreakpoint(QString(_(EVENT)), params.functionName, params.enabled);
        br.enabled = params.enabled;
        bp.setResponse(br);
    } else {
        //V8 supports only minimalistic changes in breakpoint
        //Remove the breakpoint and add again
        bp.notifyBreakpointChangeOk();
        bp.removeBreakpoint();
        BreakHandler *handler = d->engine->breakHandler();
        handler->appendBreakpoint(params);
    }

    if (bp.state() == BreakpointChangeProceeding)
        bp.notifyBreakpointChangeOk();
}

void QmlEngine::attemptBreakpointSynchronization()
{
    if (!stateAcceptsBreakpointChanges()) {
        showMessage(_("BREAKPOINT SYNCHRONIZATION NOT POSSIBLE IN CURRENT STATE"));
        return;
    }

    BreakHandler *handler = breakHandler();

    DebuggerEngine *bpOwner = isSlaveEngine() ? masterEngine() : this;
    foreach (Breakpoint bp, handler->unclaimedBreakpoints()) {
        // Take ownership of the breakpoint. Requests insertion.
        if (acceptsBreakpoint(bp))
            bp.setEngine(bpOwner);
    }

    foreach (Breakpoint bp, handler->engineBreakpoints(bpOwner)) {
        switch (bp.state()) {
        case BreakpointNew:
            // Should not happen once claimed.
            QTC_CHECK(false);
            continue;
        case BreakpointInsertRequested:
            insertBreakpoint(bp);
            continue;
        case BreakpointChangeRequested:
            changeBreakpoint(bp);
            continue;
        case BreakpointRemoveRequested:
            removeBreakpoint(bp);
            continue;
        case BreakpointChangeProceeding:
        case BreakpointInsertProceeding:
        case BreakpointRemoveProceeding:
        case BreakpointInserted:
        case BreakpointDead:
            continue;
        }
        QTC_ASSERT(false, qDebug() << "UNKNOWN STATE"  << bp << state());
    }

    DebuggerEngine::attemptBreakpointSynchronization();
}

bool QmlEngine::acceptsBreakpoint(Breakpoint bp) const
{
    if (!bp.parameters().isCppBreakpoint())
            return true;

    //If it is a Cpp Breakpoint query if the type can be also handled by the debugger client
    //TODO: enable setting of breakpoints before start of debug session
    //For now, the event breakpoint can be set after the activeDebuggerClient is known
    //This is because the older client does not support BreakpointOnQmlSignalHandler
    BreakpointType type = bp.type();
    return type == BreakpointOnQmlSignalEmit
            || type == BreakpointByFileAndLine
            || type == BreakpointAtJavaScriptThrow;
}

void QmlEngine::loadSymbols(const QString &moduleName)
{
    Q_UNUSED(moduleName)
}

void QmlEngine::loadAllSymbols()
{
}

void QmlEngine::reloadModules()
{
}

void QmlEngine::reloadSourceFiles()
{
    d->scripts(4, QList<int>(), true, QVariant());
}

void QmlEngine::requestModuleSymbols(const QString &moduleName)
{
    Q_UNUSED(moduleName)
}

bool QmlEngine::canHandleToolTip(const DebuggerToolTipContext &) const
{
    // This is processed by QML inspector, which has dependencies to
    // the qml js editor. Makes life easier.
    // FIXME: Except that there isn't any attached.
    return true;
}

void QmlEngine::assignValueInDebugger(WatchItem *item,
    const QString &expression, const QVariant &valueV)
{
    if (!expression.isEmpty()) {
        if (item->isInspect() && d->inspectorAdapter.agent()) {
            d->inspectorAdapter.agent()->assignValue(item, expression, valueV);
        } else {
            StackHandler *handler = stackHandler();
            QString expression = QString(_("%1 = %2;")).arg(expression).arg(valueV.toString());
            if (handler->isContentsValid() && handler->currentFrame().isUsable()) {
                d->evaluate(expression, false, false, handler->currentIndex());
                d->updateLocalsAndWatchers.append(d->sequence);
            } else {
                showMessage(QString(_("Cannot evaluate %1 in current stack frame")).arg(
                            expression), ConsoleOutput);
            }
        }
    }
}

void QmlEngine::updateWatchData(const QByteArray &iname)
{
//    qDebug() << "UPDATE WATCH DATA" << data.toString();
    //showStatusMessage(tr("Stopped."), 5000);
    const WatchItem *item = watchHandler()->findItem(iname);
    // invalid expressions or out of scope variables
    if (!item)
        return;

    if (item->isInspect()) {
        d->inspectorAdapter.agent()->updateWatchData(*item);
    } else {
        if (!item->name.isEmpty()) {
            if (item->isChildrenNeeded() && watchHandler()->isExpandedIName(item->iname))
                d->expandObject(item->iname, item->id);
        }
        synchronizeWatchers();
    }
}

void QmlEngine::selectWatchData(const QByteArray &iname)
{
    const WatchItem *item = watchHandler()->findItem(iname);
    if (item && item->isInspect())
        d->inspectorAdapter.agent()->watchDataSelected(item->id);
}

void QmlEngine::synchronizeWatchers()
{
    if (state() != InferiorStopOk)
        return;

    QStringList watchers = watchHandler()->watchedExpressions();

    // send watchers list
    foreach (const QString &exp, watchers) {
        StackHandler *handler = stackHandler();
        if (handler->isContentsValid() && handler->currentFrame().isUsable()) {
            d->evaluate(exp, false, false, handler->currentIndex());
            d->evaluatingExpression.insert(d->sequence, exp);
        }
    }
}

static ConsoleItem *constructLogItemTree(ConsoleItem *parent,
                                         const QVariant &result,
                                         const QString &key = QString())
{
    bool sorted = boolSetting(SortStructMembers);
    if (!result.isValid())
        return 0;

    ConsoleItem *item = new ConsoleItem(parent);
    if (result.type() == QVariant::Map) {
        if (key.isEmpty())
            item->setText(_("Object"));
        else
            item->setText(key + _(" : Object"));

        QMapIterator<QString, QVariant> i(result.toMap());
        while (i.hasNext()) {
            i.next();
            ConsoleItem *child = constructLogItemTree(item, i.value(), i.key());
            if (child)
                item->insertChild(child, sorted);
        }
    } else if (result.type() == QVariant::List) {
        if (key.isEmpty())
            item->setText(_("List"));
        else
            item->setText(QString(_("[%1] : List")).arg(key));
        QVariantList resultList = result.toList();
        for (int i = 0; i < resultList.count(); i++) {
            ConsoleItem *child = constructLogItemTree(item, resultList.at(i),
                                                          QString::number(i));
            if (child)
                item->insertChild(child, sorted);
        }
    } else if (result.canConvert(QVariant::String)) {
        item->setText(result.toString());
    } else {
        item->setText(_("Unknown Value"));
    }

    return item;
}

void QmlEngine::expressionEvaluated(quint32 queryId, const QVariant &result)
{
    if (d->queryIds.contains(queryId)) {
        d->queryIds.removeOne(queryId);
        if (auto consoleManager = ConsoleManagerInterface::instance()) {
            if (ConsoleItem *item = constructLogItemTree(consoleManager->rootItem(), result))
                consoleManager->printToConsolePane(item);
        }
    }
}

bool QmlEngine::hasCapability(unsigned cap) const
{
    return cap & (AddWatcherCapability
            | AddWatcherWhileRunningCapability
            | RunToLineCapability);
    /*ReverseSteppingCapability | SnapshotCapability
        | AutoDerefPointersCapability | DisassemblerCapability
        | RegisterCapability | ShowMemoryCapability
        | JumpToLineCapability | ReloadModuleCapability
        | ReloadModuleSymbolsCapability | BreakOnThrowAndCatchCapability
        | ReturnFromFunctionCapability
        | CreateFullBacktraceCapability
        | WatchpointCapability
        | AddWatcherCapability;*/
}

void QmlEngine::quitDebugger()
{
    d->noDebugOutputTimer.stop();
    d->automaticConnect = false;
    d->retryOnConnectFail = false;
    DebuggerEngine::quitDebugger();
}

void QmlEngine::disconnected()
{
    showMessage(tr("QML Debugger disconnected."), StatusBar);
    notifyInferiorExited();
}

void QmlEngine::documentUpdated(Document::Ptr doc)
{
    QString fileName = doc->fileName();
    if (d->pendingBreakpoints.contains(fileName)) {
        QList<Breakpoint> bps = d->pendingBreakpoints.values(fileName);
        d->pendingBreakpoints.remove(fileName);
        foreach (const Breakpoint bp, bps)
            insertBreakpoint(bp);
    }
}

void QmlEngine::updateCurrentContext()
{
    QString context;
    if (state() == InferiorStopOk) {
        context = stackHandler()->currentFrame().function;
    } else {
        QModelIndex currentIndex = inspectorView()->currentIndex();
        const WatchData *currentData = watchHandler()->watchItem(currentIndex);
        if (!currentData)
            return;
        const WatchData *parentData = watchHandler()->watchItem(currentIndex.parent());
        const WatchData *grandParentData = watchHandler()->watchItem(currentIndex.parent().parent());
        if (currentData->id != parentData->id)
            context = currentData->name;
        else if (parentData->id != grandParentData->id)
            context = parentData->name;
        else
            context = grandParentData->name;
    }

    synchronizeWatchers();

    if (auto consoleManager = ConsoleManagerInterface::instance())
        consoleManager->setContext(tr("Context:") + QLatin1Char(' ') + context);
}

void QmlEngine::executeDebuggerCommand(const QString &command, DebuggerLanguages languages)
{
    if (!(languages & QmlLanguage))
        return;

    StackHandler *handler = stackHandler();
    if (handler->isContentsValid() && handler->currentFrame().isUsable()) {
        d->evaluate(command, false, false, handler->currentIndex());
        d->debuggerCommands.append(d->sequence);
    } else {
        //Currently cannot evaluate if not in a javascript break
        d->engine->showMessage(QString(_("Cannot evaluate %1 in current stack frame")).arg(
                                   command), ConsoleOutput);
    }
}

bool QmlEngine::evaluateScript(const QString &expression)
{
    bool didEvaluate = true;
    // Evaluate expression based on engine state
    // When engine->state() == InferiorStopOk, the expression is sent to debuggerClient.
    if (state() != InferiorStopOk) {
        QModelIndex currentIndex = inspectorView()->currentIndex();
        QmlInspectorAgent *agent = d->inspectorAdapter.agent();
        quint32 queryId = agent->queryExpressionResult(watchHandler()->watchItem(currentIndex)->id,
                                                       expression);
        if (queryId) {
            d->queryIds.append(queryId);
        } else {
            didEvaluate = false;
            if (auto consoleManager = ConsoleManagerInterface::instance()) {
                consoleManager->printToConsolePane(ConsoleItem::ErrorType,
                                                   _("Error evaluating expression."));
            }
        }
    } else {
        executeDebuggerCommand(expression, QmlLanguage);
    }
    return didEvaluate;
}

void QmlEnginePrivate::updateScriptSource(const QString &fileName, int lineOffset, int columnOffset,
                                          const QString &source)
{
    QTextDocument *document = 0;
    if (sourceDocuments.contains(fileName)) {
        document = sourceDocuments.value(fileName);
    } else {
        document = new QTextDocument(this);
        sourceDocuments.insert(fileName, document);
    }

    // We're getting an unordered set of snippets that can even interleave
    // Therefore we've to carefully update the existing document

    QTextCursor cursor(document);
    for (int i = 0; i < lineOffset; ++i) {
        if (!cursor.movePosition(QTextCursor::NextBlock))
            cursor.insertBlock();
    }
    QTC_CHECK(cursor.blockNumber() == lineOffset);

    for (int i = 0; i < columnOffset; ++i) {
        if (!cursor.movePosition(QTextCursor::NextCharacter))
            cursor.insertText(QLatin1String(" "));
    }
    QTC_CHECK(cursor.positionInBlock() == columnOffset);

    QStringList lines = source.split(QLatin1Char('\n'));
    foreach (QString line, lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.remove(line.size() -1, 1);

        // line already there?
        QTextCursor existingCursor(cursor);
        existingCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        if (existingCursor.selectedText() != line)
            cursor.insertText(line);

        if (!cursor.movePosition(QTextCursor::NextBlock))
            cursor.insertBlock();
    }

    //update open editors
    QString titlePattern = tr("JS Source for %1").arg(fileName);
    //Check if there are open editors with the same title
    foreach (IDocument *doc, DocumentModel::openedDocuments()) {
        if (doc->displayName() == titlePattern) {
            updateDocument(doc, document);
            break;
        }
    }
}

bool QmlEnginePrivate::canEvaluateScript(const QString &script)
{
    interpreter.clearText();
    interpreter.appendText(script);
    return interpreter.canEvaluate();
}

void QmlEngine::connectionErrorOccurred(QDebugSupport::Error error)
{
    // this is only an error if we are already connected and something goes wrong.
    if (isConnected()) {
        if (error == QDebugSupport::RemoteClosedConnectionError)
            showMessage(tr("QML Debugger: Remote host closed connection."), StatusBar);

        if (!isSlaveEngine()) { // normal flow for slave engine when gdb exits
            notifyInferiorSpontaneousStop();
            notifyInferiorIll();
        }
    } else {
        d->connectionTimer.stop();
        connectionStartupFailed();
    }
}

void QmlEngine::clientStateChanged(QmlDebugClient::State state)
{
    QString serviceName;
    float version = 0;
    if (QmlDebugClient *client = qobject_cast<QmlDebugClient*>(sender())) {
        serviceName = client->name();
        version = client->remoteVersion();
    }

    logServiceStateChange(serviceName, version, state);
}

void QmlEngine::checkConnectionState()
{
    if (!isConnected()) {
        closeConnection();
        connectionStartupFailed();
    }
}

bool QmlEngine::isConnected() const
{
    return d->connection->isOpen();
}

void QmlEngine::showConnectionStateMessage(const QString &message)
{
    showMessage(_("QML Debugger: ") + message, LogStatus);
}

void QmlEngine::showConnectionErrorMessage(const QString &message)
{
    showMessage(_("QML Debugger: ") + message, LogError);
}

void QmlEngine::logServiceStateChange(const QString &service, float version,
                                        QmlDebugClient::State newState)
{
    switch (newState) {
    case QmlDebugClient::Unavailable: {
        showConnectionStateMessage(_("Status of \"%1\" Version: %2 changed to 'unavailable'.").
                                    arg(service).arg(QString::number(version)));
        break;
    }
    case QmlDebugClient::Enabled: {
        showConnectionStateMessage(_("Status of \"%1\" Version: %2 changed to 'enabled'.").
                                    arg(service).arg(QString::number(version)));
        break;
    }

    case QmlDebugClient::NotConnected: {
        showConnectionStateMessage(_("Status of \"%1\" Version: %2 changed to 'not connected'.").
                                    arg(service).arg(QString::number(version)));
        break;
    }
    }
}

void QmlEngine::logServiceActivity(const QString &service, const QString &logMessage)
{
    showMessage(service + QLatin1Char(' ') + logMessage, LogDebug);
}

void QmlEnginePrivate::connect()
{
    engine->showMessage(_(CONNECT), LogInput);
    sendMessage(packMessage(CONNECT));
}

void QmlEnginePrivate::disconnect()
{
    //    { "seq"     : <number>,
    //      "type"    : "request",
    //      "command" : "disconnect",
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(DISCONNECT));

    const QByteArray msg = QJsonDocument(jsonVal).toJson(QJsonDocument::Compact);
    engine->showMessage(QString::fromUtf8(msg), LogInput);
    sendMessage(packMessage(DISCONNECT, msg));
}

void QmlEnginePrivate::continueDebugging(StepAction action)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "continue",
    //      "arguments" : { "stepaction" : <"in", "next" or "out">,
    //                      "stepcount"  : <number of steps (default 1)>
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(CONTINEDEBUGGING));

    if (action != Continue) {
        QJsonObject args;
        switch (action) {
        case StepIn:
            args.insert(_(STEPACTION), _(IN));
            break;
        case StepOut:
            args.insert(_(STEPACTION), _(OUT));
            break;
        case Next:
            args.insert(_(STEPACTION), _(NEXT));
            break;
        default:break;
        }

        jsonVal.insert(_(ARGUMENTS), args);
    }
    sendAndLogV8Request(jsonVal);
    previousStepAction = action;
}

void QmlEnginePrivate::evaluate(const QString expr, bool global,
                                bool disableBreak, int frame, bool addContext)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "evaluate",
    //      "arguments" : { "expression"    : <expression to evaluate>,
    //                      "frame"         : <number>,
    //                      "global"        : <boolean>,
    //                      "disable_break" : <boolean>,
    //                      "additional_context" : [
    //                           { "name" : <name1>, "handle" : <handle1> },
    //                           { "name" : <name2>, "handle" : <handle2> },
    //                           ...
    //                      ]
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(EVALUATE));

    QJsonObject args {
        { _(EXPRESSION), expr }
    };

    if (frame != -1)
        args.insert(_(FRAME), frame);

    if (global)
        args.insert(_(GLOBAL), global);

    if (disableBreak)
        args.insert(_(DISABLE_BREAK), disableBreak);

    if (addContext) {
        WatchHandler *watchHandler = engine->watchHandler();
        QAbstractItemModel *watchModel = watchHandler->model();
        int rowCount = watchModel->rowCount();

        QJsonArray ctxtList;
        while (rowCount) {
            QModelIndex index = watchModel->index(--rowCount, 0);
            const WatchData *data = watchHandler->watchItem(index);
            const QJsonObject ctxt {
                { _(NAME), data->name },
                { _(HANDLE), int(data->id) }
            };

            ctxtList.push_front(ctxt);
        }

        args.insert(_(ADDITIONAL_CONTEXT), ctxtList);
    }

    jsonVal.insert(_(ARGUMENTS), args);

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::lookup(QList<int> handles, bool includeSource)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "lookup",
    //      "arguments" : { "handles"       : <array of handles>,
    //                      "includeSource" : <boolean indicating whether
    //                                          the source will be included when
    //                                          script objects are returned>,
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(LOOKUP));

    QJsonObject args;

    QJsonArray array;
    foreach (int handle, handles)
        array.push_back(handle);
    args.insert(_(HANDLES), array);

    if (includeSource)
        args.insert(_(INCLUDESOURCE), includeSource);

    jsonVal.insert(_(ARGUMENTS), args);

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::backtrace(int fromFrame, int toFrame, bool bottom)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "backtrace",
    //      "arguments" : { "fromFrame" : <number>
    //                      "toFrame" : <number>
    //                      "bottom" : <boolean, set to true if the bottom of the
    //                          stack is requested>
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(BACKTRACE));

    QJsonObject args;

    if (fromFrame != -1)
        args.insert(_(FROMFRAME), fromFrame);

    if (toFrame != -1)
        args.insert(_(TOFRAME), toFrame);

    if (bottom)
        args.insert(_(BOTTOM), bottom);

    jsonVal.insert(_(ARGUMENTS), args);

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::frame(int number)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "frame",
    //      "arguments" : { "number" : <frame number>
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(FRAME));

    if (number != -1) {
        const QJsonObject args {
            { _(NUMBER), number }
        };

        jsonVal.insert(_(ARGUMENTS), args);
    }

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::scope(int number, int frameNumber)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "scope",
    //      "arguments" : { "number" : <scope number>
    //                      "frameNumber" : <frame number, optional uses selected
    //                                      frame if missing>
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(SCOPE));

    if (number != -1) {
        QJsonObject args {
            { _(NUMBER), number }
        };

        if (frameNumber != -1)
            args.insert(_(FRAMENUMBER), frameNumber);

        jsonVal.insert(_(ARGUMENTS), args);
    }

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::scripts(int types, const QList<int> ids, bool includeSource,
                               const QVariant filter)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "scripts",
    //      "arguments" : { "types"         : <types of scripts to retrieve
    //                                           set bit 0 for native scripts
    //                                           set bit 1 for extension scripts
    //                                           set bit 2 for normal scripts
    //                                         (default is 4 for normal scripts)>
    //                      "ids"           : <array of id's of scripts to return. If this is not specified all scripts are requrned>
    //                      "includeSource" : <boolean indicating whether the source code should be included for the scripts returned>
    //                      "filter"        : <string or number: filter string or script id.
    //                                         If a number is specified, then only the script with the same number as its script id will be retrieved.
    //                                         If a string is specified, then only scripts whose names contain the filter string will be retrieved.>
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(SCRIPTS));

    QJsonObject args {
        { _(TYPES), types }
    };

    if (ids.count()) {
        QJsonArray array;
        foreach (int id, ids) {
            array.push_back(id);
        }
        args.insert(_(IDS), array);
    }

    if (includeSource)
        args.insert(_(INCLUDESOURCE), includeSource);

    QJsonValue filterValue;
    if (filter.type() == QVariant::String)
        filterValue = filter.toString();
    else if (filter.type() == QVariant::Int)
        filterValue = filter.toInt();
    else
        QTC_CHECK(!filter.isValid());

    args.insert(_(FILTER), filterValue);

    jsonVal.insert(_(ARGUMENTS), args);

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::setBreakpoint(const QString type, const QString target,
                                     bool enabled, int line, int column,
                                     const QString condition, int ignoreCount)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "setbreakpoint",
    //      "arguments" : { "type"        : <"function" or "script" or "scriptId" or "scriptRegExp">
    //                      "target"      : <function expression or script identification>
    //                      "line"        : <line in script or function>
    //                      "column"      : <character position within the line>
    //                      "enabled"     : <initial enabled state. True or false, default is true>
    //                      "condition"   : <string with break point condition>
    //                      "ignoreCount" : <number specifying the number of break point hits to ignore, default value is 0>
    //                    }
    //    }
    if (type == _(EVENT)) {
        QByteArray params;
        QmlDebugStream rs(&params, QIODevice::WriteOnly);
        rs <<  target.toUtf8() << enabled;
        engine->showMessage(QString(_("%1 %2 %3")).arg(_(BREAKONSIGNAL), target, enabled ? _("enabled") : _("disabled")), LogInput);
        sendMessage(packMessage(BREAKONSIGNAL, params));

    } else {
        QJsonObject jsonVal = initObject();
        jsonVal.insert(_(COMMAND), _(SETBREAKPOINT));

        QJsonObject args {
            { _(TYPE), type },
            { _(ENABLED), enabled }
        };
        if (type == _(SCRIPTREGEXP))
            args.insert(_(TARGET), Utils::FileName::fromString(target).fileName());
        else
            args.insert(_(TARGET), target);

        if (line)
            args.insert(_(LINE), line - 1);

        if (column)
            args.insert(_(COLUMN), column - 1);

        if (!condition.isEmpty())
            args.insert(_(CONDITION), condition);

        if (ignoreCount != -1)
            args.insert(_(IGNORECOUNT), ignoreCount);

        jsonVal.insert(_(ARGUMENTS), args);

        sendAndLogV8Request(jsonVal);
    }
}

void QmlEnginePrivate::clearBreakpoint(int breakpoint)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "clearbreakpoint",
    //      "arguments" : { "breakpoint" : <number of the break point to clear>
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(CLEARBREAKPOINT));

    QJsonObject args {
        { _(BREAKPOINT), breakpoint }
    };

    jsonVal.insert(_(ARGUMENTS), args);

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::setExceptionBreak(Exceptions type, bool enabled)
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "setexceptionbreak",
    //      "arguments" : { "type"    : <string: "all", or "uncaught">,
    //                      "enabled" : <optional bool: enables the break type if true>
    //                    }
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(SETEXCEPTIONBREAK));

    QJsonObject args;

    if (type == AllExceptions)
        args.insert(_(TYPE), _(ALL));
    //Not Supported
    //    else if (type == UncaughtExceptions)
    //        args.setProperty(_(TYPE),QScriptValue(_(UNCAUGHT)));

    if (enabled)
        args.insert(_(ENABLED), enabled);

    jsonVal.insert(_(ARGUMENTS), args);

    sendAndLogV8Request(jsonVal);
}

void QmlEnginePrivate::version()
{
    //    { "seq"       : <number>,
    //      "type"      : "request",
    //      "command"   : "version",
    //    }
    QJsonObject jsonVal = initObject();
    jsonVal.insert(_(COMMAND), _(VERSION));

    sendAndLogV8Request(jsonVal);
}

QVariant valueFromRef(int handle, const QVariant &refsVal, bool *success)
{
    *success = false;
    QVariant variant;
    const QVariantList refs = refsVal.toList();
    foreach (const QVariant &ref, refs) {
        const QVariantMap refData = ref.toMap();
        if (refData.value(_(HANDLE)).toInt() == handle) {
            variant = refData;
            *success = true;
            break;
        }
    }
    return variant;
}

QmlV8ObjectData extractData(const QVariant &data, const QVariant &refsVal)
{
    //    { "handle" : <handle>,
    //      "type"   : <"undefined", "null", "boolean", "number", "string", "object", "function" or "frame">
    //    }

    //    {"handle":<handle>,"type":"undefined"}

    //    {"handle":<handle>,"type":"null"}

    //    { "handle":<handle>,
    //      "type"  : <"boolean", "number" or "string">
    //      "value" : <JSON encoded value>
    //    }

    //    {"handle":7,"type":"boolean","value":true}

    //    {"handle":8,"type":"number","value":42}

    //    { "handle"              : <handle>,
    //      "type"                : "object",
    //      "className"           : <Class name, ECMA-262 property [[Class]]>,
    //      "constructorFunction" : {"ref":<handle>},
    //      "protoObject"         : {"ref":<handle>},
    //      "prototypeObject"     : {"ref":<handle>},
    //      "properties" : [ {"name" : <name>,
    //                        "ref"  : <handle>
    //                       },
    //                       ...
    //                     ]
    //    }

    //        { "handle" : <handle>,
    //          "type"                : "function",
    //          "className"           : "Function",
    //          "constructorFunction" : {"ref":<handle>},
    //          "protoObject"         : {"ref":<handle>},
    //          "prototypeObject"     : {"ref":<handle>},
    //          "name"                : <function name>,
    //          "inferredName"        : <inferred function name for anonymous functions>
    //          "source"              : <function source>,
    //          "script"              : <reference to function script>,
    //          "scriptId"            : <id of function script>,
    //          "position"            : <function begin position in script>,
    //          "line"                : <function begin source line in script>,
    //          "column"              : <function begin source column in script>,
    //          "properties" : [ {"name" : <name>,
    //                            "ref"  : <handle>
    //                           },
    //                           ...
    //                         ]
    //        }

    QmlV8ObjectData objectData;
    const QVariantMap dataMap = data.toMap();

    objectData.name = dataMap.value(_(NAME)).toByteArray();

    if (dataMap.contains(_(REF))) {
        objectData.handle = dataMap.value(_(REF)).toInt();
        bool success;
        QVariant dataFromRef = valueFromRef(objectData.handle, refsVal, &success);
        if (success) {
            QmlV8ObjectData data = extractData(dataFromRef, refsVal);
            objectData.type = data.type;
            objectData.value = data.value;
            objectData.properties = data.properties;
        }
    } else {
        objectData.handle = dataMap.value(_(HANDLE)).toInt();
        QString type = dataMap.value(_(TYPE)).toString();

        if (type == _("undefined")) {
            objectData.type = QByteArray("undefined");
            objectData.value = QVariant(_("undefined"));

        } else if (type == _("null")) {
            objectData.type = QByteArray("null");
            objectData.value= QVariant(_("null"));

        } else if (type == _("boolean")) {
            objectData.type = QByteArray("boolean");
            objectData.value = dataMap.value(_(VALUE));

        } else if (type == _("number")) {
            objectData.type = QByteArray("number");
            objectData.value = dataMap.value(_(VALUE));

        } else if (type == _("string")) {
            objectData.type = QByteArray("string");
            objectData.value = dataMap.value(_(VALUE));

        } else if (type == _("object")) {
            objectData.type = QByteArray("object");
            objectData.value = dataMap.value(_("className"));
            objectData.properties = dataMap.value(_("properties")).toList();

        } else if (type == _("function")) {
            objectData.type = QByteArray("function");
            objectData.value = dataMap.value(_(NAME));
            objectData.properties = dataMap.value(_("properties")).toList();

        } else if (type == _("script")) {
            objectData.type = QByteArray("script");
            objectData.value = dataMap.value(_(NAME));
        }
    }

    return objectData;
}

void QmlEnginePrivate::clearCache()
{
    currentFrameScopes.clear();
    updateLocalsAndWatchers.clear();
}

QByteArray QmlEnginePrivate::packMessage(const QByteArray &type, const QByteArray &message)
{
    SDEBUG(message);
    QByteArray request;
    QmlDebugStream rs(&request, QIODevice::WriteOnly);
    QByteArray cmd = V8DEBUG;
    rs << cmd << type << message;
    return request;
}

QJsonObject QmlEnginePrivate::initObject()
{
    return QJsonObject {
        {_(SEQ), ++sequence},
        {_(TYPE), _(REQUEST)}
    };
}

void QmlEnginePrivate::sendAndLogV8Request(const QJsonObject &request)
{
    const QByteArray msg = QJsonDocument(request).toJson(QJsonDocument::Compact);
    engine->showMessage(QString::fromLatin1("%1 %2").arg(_(V8REQUEST), QString::fromUtf8(msg)), LogInput);
    sendMessage(packMessage(V8REQUEST, msg));
}

void QmlEnginePrivate::expandObject(const QByteArray &iname, quint64 objectId)
{
    if (objectId == 0) {
        //We may have got the global object
        const WatchItem *watch = engine->watchHandler()->findItem(iname);
        if (watch->value == QLatin1String("global")) {
            StackHandler *stackHandler = engine->stackHandler();
            if (stackHandler->isContentsValid() && stackHandler->currentFrame().isUsable()) {
                evaluate(watch->name, false, false, stackHandler->currentIndex());
                evaluatingExpression.insert(sequence, QLatin1String(iname));
            }
            return;
        }
    }
    localsAndWatchers.insertMulti(objectId, iname);
    lookup(QList<int>() << objectId);
}

void QmlEnginePrivate::messageReceived(const QByteArray &data)
{
    QmlDebugStream ds(data);
    QByteArray command;
    ds >> command;

    if (command == V8DEBUG) {
        QByteArray type;
        QByteArray response;
        ds >> type >> response;

        engine->showMessage(QLatin1String(type), LogOutput);
        if (type == CONNECT) {
            //debugging session started

        } else if (type == INTERRUPT) {
            //debug break requested

        } else if (type == BREAKONSIGNAL) {
            //break on signal handler requested

        } else if (type == V8MESSAGE) {
            const QString responseString = QLatin1String(response);
            SDEBUG(responseString);
            engine->showMessage(QLatin1String(V8MESSAGE) + QLatin1Char(' ') + responseString, LogOutput);

            const QVariantMap resp =
                    QJsonDocument::fromJson(responseString.toUtf8()).toVariant().toMap();

            const QString type(resp.value(_(TYPE)).toString());

            if (type == _("response")) {

                bool success = resp.value(_("success")).toBool();
                if (!success) {
                    SDEBUG("Request was unsuccessful");
                }

                const QString debugCommand(resp.value(_(COMMAND)).toString());

                if (debugCommand == _(DISCONNECT)) {
                    //debugging session ended

                } else if (debugCommand == _(CONTINEDEBUGGING)) {
                    //do nothing, wait for next break

                } else if (debugCommand == _(BACKTRACE)) {
                    if (success)
                        handleBacktrace(resp.value(_(BODY)), resp.value(_(REFS)));

                } else if (debugCommand == _(LOOKUP)) {
                    if (success)
                        handleLookup(resp.value(_(BODY)), resp.value(_(REFS)));

                } else if (debugCommand == _(EVALUATE)) {
                    int seq = resp.value(_("request_seq")).toInt();
                    if (success) {
                        handleEvaluate(seq, success, resp.value(_(BODY)), resp.value(_(REFS)));
                    } else {
                        QVariantMap map;
                        map.insert(_(TYPE), QVariant(_("string")));
                        map.insert(_(VALUE), resp.value(_("message")));
                        handleEvaluate(seq, success, QVariant(map), QVariant());
                    }

                } else if (debugCommand == _(SETBREAKPOINT)) {
                    //                { "seq"         : <number>,
                    //                  "type"        : "response",
                    //                  "request_seq" : <number>,
                    //                  "command"     : "setbreakpoint",
                    //                  "body"        : { "type"       : <"function" or "script">
                    //                                    "breakpoint" : <break point number of the new break point>
                    //                                  }
                    //                  "running"     : <is the VM running after sending this response>
                    //                  "success"     : true
                    //                }

                    int seq = resp.value(_("request_seq")).toInt();
                    const QVariantMap breakpointData = resp.value(_(BODY)).toMap();
                    int index = breakpointData.value(_("breakpoint")).toInt();

                    if (breakpointsSync.contains(seq)) {
                        BreakpointModelId id = breakpointsSync.take(seq);
                        breakpoints.insert(id, index);

                        //Is actual position info present? Then breakpoint was
                        //accepted
                        const QVariantList actualLocations =
                                breakpointData.value(_("actual_locations")).toList();
                        if (actualLocations.count()) {
                            //The breakpoint requested line should be same as
                            //actual line
                            BreakHandler *handler = engine->breakHandler();
                            Breakpoint bp = handler->breakpointById(id);
                            if (bp.state() != BreakpointInserted) {
                                BreakpointResponse br = bp.response();
                                br.lineNumber = breakpointData.value(_("line")).toInt() + 1;
                                bp.setResponse(br);
                                bp.notifyBreakpointInsertOk();
                            }
                        }


                    } else {
                        breakpointsTemp.append(index);
                    }


                } else if (debugCommand == _(CLEARBREAKPOINT)) {
                    // DO NOTHING

                } else if (debugCommand == _(SETEXCEPTIONBREAK)) {
                    //                { "seq"               : <number>,
                    //                  "type"              : "response",
                    //                  "request_seq" : <number>,
                    //                  "command"     : "setexceptionbreak",
                    //                  "body"        : { "type"    : <string: "all" or "uncaught" corresponding to the request.>,
                    //                                    "enabled" : <bool: true if the break type is currently enabled as a result of the request>
                    //                                  }
                    //                  "running"     : true
                    //                  "success"     : true
                    //                }


                } else if (debugCommand == _(FRAME)) {
                    if (success)
                        handleFrame(resp.value(_(BODY)), resp.value(_(REFS)));

                } else if (debugCommand == _(SCOPE)) {
                    if (success)
                        handleScope(resp.value(_(BODY)), resp.value(_(REFS)));

                } else if (debugCommand == _(SCRIPTS)) {
                    //                { "seq"         : <number>,
                    //                  "type"        : "response",
                    //                  "request_seq" : <number>,
                    //                  "command"     : "scripts",
                    //                  "body"        : [ { "name"             : <name of the script>,
                    //                                      "id"               : <id of the script>
                    //                                      "lineOffset"       : <line offset within the containing resource>
                    //                                      "columnOffset"     : <column offset within the containing resource>
                    //                                      "lineCount"        : <number of lines in the script>
                    //                                      "data"             : <optional data object added through the API>
                    //                                      "source"           : <source of the script if includeSource was specified in the request>
                    //                                      "sourceStart"      : <first 80 characters of the script if includeSource was not specified in the request>
                    //                                      "sourceLength"     : <total length of the script in characters>
                    //                                      "scriptType"       : <script type (see request for values)>
                    //                                      "compilationType"  : < How was this script compiled:
                    //                                                               0 if script was compiled through the API
                    //                                                               1 if script was compiled through eval
                    //                                                            >
                    //                                      "evalFromScript"   : <if "compilationType" is 1 this is the script from where eval was called>
                    //                                      "evalFromLocation" : { line   : < if "compilationType" is 1 this is the line in the script from where eval was called>
                    //                                                             column : < if "compilationType" is 1 this is the column in the script from where eval was called>
                    //                                  ]
                    //                  "running"     : <is the VM running after sending this response>
                    //                  "success"     : true
                    //                }

                    if (success) {
                        const QVariantList body = resp.value(_(BODY)).toList();

                        QStringList sourceFiles;
                        for (int i = 0; i < body.size(); ++i) {
                            const QVariantMap entryMap = body.at(i).toMap();
                            const int lineOffset = entryMap.value(QLatin1String("lineOffset")).toInt();
                            const int columnOffset = entryMap.value(QLatin1String("columnOffset")).toInt();
                            const QString name = entryMap.value(QLatin1String("name")).toString();
                            const QString source = entryMap.value(QLatin1String("source")).toString();

                            if (name.isEmpty())
                                continue;

                            if (!sourceFiles.contains(name))
                                sourceFiles << name;

                            updateScriptSource(name, lineOffset, columnOffset, source);
                        }

                        QMap<QString,QString> files;
                        foreach (const QString &file, sourceFiles) {
                            QString shortName = file;
                            QString fullName = engine->toFileInProject(file);
                            files.insert(shortName, fullName);
                        }

                        engine->sourceFilesHandler()->setSourceFiles(files);
                        //update open editors
                    }
                } else if (debugCommand == _(VERSION)) {
                    engine->showMessage(QString(_("Using V8 Version: %1")).arg(
                                             resp.value(_(BODY)).toMap().
                                             value(_("V8Version")).toString()), LogOutput);

                } else {
                    // DO NOTHING
                }

            } else if (type == _(EVENT)) {
                const QString eventType(resp.value(_(EVENT)).toString());

                if (eventType == _("break")) {
                    const QVariantMap breakData = resp.value(_(BODY)).toMap();
                    const QString invocationText = breakData.value(_("invocationText")).toString();
                    const QString scriptUrl = breakData.value(_("script")).toMap().value(_("name")).toString();
                    const QString sourceLineText = breakData.value(_("sourceLineText")).toString();

                    bool inferiorStop = true;

                    QList<int> v8BreakpointIds;
                    {
                        const QVariantList v8BreakpointIdList = breakData.value(_("breakpoints")).toList();
                        foreach (const QVariant &breakpointId, v8BreakpointIdList)
                            v8BreakpointIds << breakpointId.toInt();
                    }

                    if (!v8BreakpointIds.isEmpty() && invocationText.startsWith(_("[anonymous]()"))
                            && scriptUrl.endsWith(_(".qml"))
                            && sourceLineText.trimmed().startsWith(QLatin1Char('('))) {

                        // we hit most likely the anonymous wrapper function automatically generated for bindings
                        // -> relocate the breakpoint to column: 1 and continue

                        int newColumn = sourceLineText.indexOf(QLatin1Char('(')) + 1;
                        BreakHandler *handler = engine->breakHandler();

                        foreach (int v8Id, v8BreakpointIds) {
                            const BreakpointModelId id = breakpoints.key(v8Id);
                            Breakpoint bp = handler->breakpointById(id);
                            if (bp.isValid()) {
                                const BreakpointParameters &params = bp.parameters();

                                clearBreakpoint(v8Id);
                                setBreakpoint(QString(_(SCRIPTREGEXP)),
                                                 params.fileName,
                                                 params.enabled,
                                                 params.lineNumber,
                                                 newColumn,
                                                 QString(QString::fromLatin1(params.condition)),
                                                 params.ignoreCount);
                                breakpointsSync.insert(sequence, id);
                            }
                        }
                        continueDebugging(Continue);
                        inferiorStop = false;
                    }

                    //Skip debug break if this is an internal function
                    if (sourceLineText == _(INTERNAL_FUNCTION)) {
                        continueDebugging(previousStepAction);
                        inferiorStop = false;
                    }

                    if (inferiorStop) {
                        //Update breakpoint data
                        BreakHandler *handler = engine->breakHandler();
                        foreach (int v8Id, v8BreakpointIds) {
                            const BreakpointModelId id = breakpoints.key(v8Id);
                            Breakpoint bp = handler->breakpointById(id);
                            if (bp) {
                                BreakpointResponse br = bp.response();
                                if (br.functionName.isEmpty()) {
                                    br.functionName = invocationText;
                                    bp.setResponse(br);
                                }
                                if (bp.state() != BreakpointInserted) {
                                    br.lineNumber = breakData.value(
                                                _("sourceLine")).toInt() + 1;
                                    bp.setResponse(br);
                                    bp.notifyBreakpointInsertOk();
                                }
                            }
                        }

                        if (engine->state() == InferiorRunOk) {
                            foreach (const QVariant &breakpointId, v8BreakpointIds) {
                                if (breakpointsTemp.contains(breakpointId.toInt()))
                                    clearBreakpoint(breakpointId.toInt());
                            }
                            engine->notifyInferiorSpontaneousStop();
                            backtrace();
                        } else if (engine->state() == InferiorStopOk) {
                            backtrace();
                        }
                    }

                } else if (eventType == _("exception")) {
                    const QVariantMap body = resp.value(_(BODY)).toMap();
                    int lineNumber = body.value(_("sourceLine")).toInt() + 1;

                    const QVariantMap script = body.value(_("script")).toMap();
                    QUrl fileUrl(script.value(_(NAME)).toString());
                    QString filePath = engine->toFileInProject(fileUrl);

                    const QVariantMap exception = body.value(_("exception")).toMap();
                    QString errorMessage = exception.value(_("text")).toString();

                    QStringList messages = highlightExceptionCode(lineNumber, filePath, errorMessage);
                    foreach (const QString msg, messages)
                        engine->showMessage(msg, ConsoleOutput);

                    if (engine->state() == InferiorRunOk) {
                        engine->notifyInferiorSpontaneousStop();
                        backtrace();
                    }

                    if (engine->state() == InferiorStopOk)
                        backtrace();

                } else if (eventType == _("afterCompile")) {
                    //Currently break point relocation is disabled.
                    //Uncomment the line below when it will be enabled.
//                    listBreakpoints();
                }

                //Sometimes we do not get event type!
                //This is most probably due to a wrong eval expression.
                //Redirect output to console.
                if (eventType.isEmpty()) {
                     bool success = resp.value(_("success")).toBool();
                     QVariantMap map;
                     map.insert(_(TYPE), QVariant(_("string")));
                     map.insert(_(VALUE), resp.value(_("message")));
                     //Since there is no sequence value, best estimate is
                     //last sequence value
                     handleEvaluate(sequence, success, QVariant(map), QVariant());
                }

            } //EVENT
        } //V8MESSAGE

    } else {
        //DO NOTHING
    }
}

void QmlEnginePrivate::handleBacktrace(const QVariant &bodyVal, const QVariant &refsVal)
{
    //    { "seq"         : <number>,
    //      "type"        : "response",
    //      "request_seq" : <number>,
    //      "command"     : "backtrace",
    //      "body"        : { "fromFrame" : <number>
    //                        "toFrame" : <number>
    //                        "totalFrames" : <number>
    //                        "frames" : <array of frames - see frame request for details>
    //                      }
    //      "running"     : <is the VM running after sending this response>
    //      "success"     : true
    //    }

    const QVariantMap body = bodyVal.toMap();
    const QVariantList frames = body.value(_("frames")).toList();

    int fromFrameIndex = body.value(_("fromFrame")).toInt();

    QTC_ASSERT(0 == fromFrameIndex, return);

    StackHandler *stackHandler = engine->stackHandler();
    StackFrames stackFrames;
    int i = 0;
    stackIndexLookup.clear();
    foreach (const QVariant &frame, frames) {
        StackFrame stackFrame = extractStackFrame(frame, refsVal);
        if (stackFrame.level < 0)
            continue;
        stackIndexLookup.insert(i, stackFrame.level);
        stackFrame.level = i;
        stackFrames << stackFrame;
        i++;
    }
    stackHandler->setFrames(stackFrames);

    //Populate locals and watchers wrt top frame
    //Update all Locals visible in current scope
    //Traverse the scope chain and store the local properties
    //in a list and show them in the Locals Window.
    handleFrame(frames.value(0), refsVal);
}

StackFrame QmlEnginePrivate::extractStackFrame(const QVariant &bodyVal, const QVariant &refsVal)
{
    //    { "seq"         : <number>,
    //      "type"        : "response",
    //      "request_seq" : <number>,
    //      "command"     : "frame",
    //      "body"        : { "index"          : <frame number>,
    //                        "receiver"       : <frame receiver>,
    //                        "func"           : <function invoked>,
    //                        "script"         : <script for the function>,
    //                        "constructCall"  : <boolean indicating whether the function was called as constructor>,
    //                        "debuggerFrame"  : <boolean indicating whether this is an internal debugger frame>,
    //                        "arguments"      : [ { name: <name of the argument - missing of anonymous argument>,
    //                                               value: <value of the argument>
    //                                             },
    //                                             ... <the array contains all the arguments>
    //                                           ],
    //                        "locals"         : [ { name: <name of the local variable>,
    //                                               value: <value of the local variable>
    //                                             },
    //                                             ... <the array contains all the locals>
    //                                           ],
    //                        "position"       : <source position>,
    //                        "line"           : <source line>,
    //                        "column"         : <source column within the line>,
    //                        "sourceLineText" : <text for current source line>,
    //                        "scopes"         : [ <array of scopes, see scope request below for format> ],

    //                      }
    //      "running"     : <is the VM running after sending this response>
    //      "success"     : true
    //    }

    const QVariantMap body = bodyVal.toMap();

    StackFrame stackFrame;
    stackFrame.level = body.value(_("index")).toInt();
    //Do not insert the frame corresponding to the internal function
    if (body.value(QLatin1String("sourceLineText")) == QLatin1String(INTERNAL_FUNCTION)) {
        stackFrame.level = -1;
        return stackFrame;
    }

    QmlV8ObjectData objectData = extractData(body.value(_("func")), refsVal);
    QString functionName = objectData.value.toString();
    if (functionName.isEmpty())
        functionName = tr("Anonymous Function");
    stackFrame.function = functionName;

    objectData = extractData(body.value(_("script")), refsVal);
    stackFrame.file = engine->toFileInProject(objectData.value.toString());
    stackFrame.usable = QFileInfo(stackFrame.file).isReadable();

    objectData = extractData(body.value(_("receiver")), refsVal);
    stackFrame.to = objectData.value.toString();

    stackFrame.line = body.value(_("line")).toInt() + 1;

    return stackFrame;
}

void QmlEnginePrivate::handleFrame(const QVariant &bodyVal, const QVariant &refsVal)
{
    //    { "seq"         : <number>,
    //      "type"        : "response",
    //      "request_seq" : <number>,
    //      "command"     : "frame",
    //      "body"        : { "index"          : <frame number>,
    //                        "receiver"       : <frame receiver>,
    //                        "func"           : <function invoked>,
    //                        "script"         : <script for the function>,
    //                        "constructCall"  : <boolean indicating whether the function was called as constructor>,
    //                        "debuggerFrame"  : <boolean indicating whether this is an internal debugger frame>,
    //                        "arguments"      : [ { name: <name of the argument - missing of anonymous argument>,
    //                                               value: <value of the argument>
    //                                             },
    //                                             ... <the array contains all the arguments>
    //                                           ],
    //                        "locals"         : [ { name: <name of the local variable>,
    //                                               value: <value of the local variable>
    //                                             },
    //                                             ... <the array contains all the locals>
    //                                           ],
    //                        "position"       : <source position>,
    //                        "line"           : <source line>,
    //                        "column"         : <source column within the line>,
    //                        "sourceLineText" : <text for current source line>,
    //                        "scopes"         : [ <array of scopes, see scope request below for format> ],

    //                      }
    //      "running"     : <is the VM running after sending this response>
    //      "success"     : true
    //    }
    QVariantMap currentFrame = bodyVal.toMap();

    StackHandler *stackHandler = engine->stackHandler();
    WatchHandler * watchHandler = engine->watchHandler();
    watchHandler->notifyUpdateStarted();
    clearCache();

    const int frameIndex = stackHandler->currentIndex();
    QSet<QByteArray> expandedInames = watchHandler->expandedINames();
    QHash<quint64, QByteArray> handlesToLookup;
    // Store handles of all expanded watch data
    foreach (const QByteArray &iname, expandedInames) {
        const WatchItem *item = watchHandler->findItem(iname);
        if (item && item->isLocal())
            handlesToLookup.insert(item->id, iname);
    }
    if (frameIndex < 0)
        return;
    const StackFrame frame = stackHandler->currentFrame();
    if (!frame.isUsable())
        return;

    //Set "this" variable
    {
        auto item = new WatchItem("local.this", QLatin1String("this"));
        QmlV8ObjectData objectData = extractData(currentFrame.value(_("receiver")), refsVal);
        item->id = objectData.handle;
        item->type = objectData.type;
        item->value = objectData.value.toString();
        item->setHasChildren(objectData.properties.count());
        // In case of global object, we do not get children
        // Set children nevertheless and query later.
        if (item->value == QLatin1String("global")) {
            item->setHasChildren(true);
            item->id = 0;
        }
        watchHandler->insertItem(item);
    }

    const QVariantList scopes = currentFrame.value(_("scopes")).toList();
    foreach (const QVariant &scope, scopes) {
        //Do not query for global types (0)
        //Showing global properties increases clutter.
        if (scope.toMap().value(_("type")).toInt() == 0)
            continue;
        int scopeIndex = scope.toMap().value(_("index")).toInt();
        currentFrameScopes.append(scopeIndex);
        this->scope(scopeIndex);
    }
    engine->gotoLocation(stackHandler->currentFrame());

    // Expand watch data that were previously expanded
    QHash<quint64, QByteArray>::const_iterator itEnd = handlesToLookup.constEnd();
    for (QHash<quint64, QByteArray>::const_iterator it = handlesToLookup.constBegin(); it != itEnd; ++it)
        expandObject(it.value(), it.key());
    engine->stackFrameCompleted();
}

void QmlEnginePrivate::handleScope(const QVariant &bodyVal, const QVariant &refsVal)
{
    //    { "seq"         : <number>,
    //      "type"        : "response",
    //      "request_seq" : <number>,
    //      "command"     : "scope",
    //      "body"        : { "index"      : <index of this scope in the scope chain. Index 0 is the top scope
    //                                        and the global scope will always have the highest index for a
    //                                        frame>,
    //                        "frameIndex" : <index of the frame>,
    //                        "type"       : <type of the scope:
    //                                         0: Global
    //                                         1: Local
    //                                         2: With
    //                                         3: Closure
    //                                         4: Catch >,
    //                        "object"     : <the scope object defining the content of the scope.
    //                                        For local and closure scopes this is transient objects,
    //                                        which has a negative handle value>
    //                      }
    //      "running"     : <is the VM running after sending this response>
    //      "success"     : true
    //    }
    QVariantMap bodyMap = bodyVal.toMap();

    //Check if the frameIndex is same as current Stack Index
    StackHandler *stackHandler = engine->stackHandler();
    if (bodyMap.value(_("frameIndex")).toInt() != stackHandler->currentIndex())
        return;

    QmlV8ObjectData objectData = extractData(bodyMap.value(_("object")), refsVal);

    QList<int> handlesToLookup;
    foreach (const QVariant &property, objectData.properties) {
        QmlV8ObjectData localData = extractData(property, refsVal);
        auto item = new WatchItem;
        item->exp = localData.name;
        //Check for v8 specific local data
        if (item->exp.startsWith('.') || item->exp.isEmpty())
            continue;

        item->name = QLatin1String(item->exp);
        item->iname = QByteArray("local.") + item->exp;

        int handle = localData.handle;
        if (localData.value.isValid()) {
            item->id = handle;
            item->type = localData.type;
            item->value = localData.value.toString();
            item->setHasChildren(localData.properties.count());
            engine->watchHandler()->insertItem(item);
        } else {
            handlesToLookup << handle;
            localsAndWatchers.insertMulti(handle, item->exp);
        }
    }

    if (!handlesToLookup.isEmpty())
        lookup(handlesToLookup);
    else
        engine->watchHandler()->notifyUpdateFinished();
}

ConsoleItem *constructLogItemTree(ConsoleItem *parent,
                                  const QmlV8ObjectData &objectData,
                                  const QVariant &refsVal)
{
    bool sorted = boolSetting(SortStructMembers);
    if (!objectData.value.isValid())
        return 0;

    QString text;
    if (objectData.name.isEmpty())
        text = objectData.value.toString();
    else
        text = QString(_("%1: %2")).arg(QString::fromLatin1(objectData.name))
                .arg(objectData.value.toString());

    ConsoleItem *item = new ConsoleItem(parent, ConsoleItem::UndefinedType, text);

    QSet<QString> childrenFetched;
    foreach (const QVariant &property, objectData.properties) {
        const QmlV8ObjectData childObjectData = extractData(property, refsVal);
        if (childObjectData.handle == objectData.handle)
            continue;
        ConsoleItem *child = constructLogItemTree(item, childObjectData, refsVal);
        if (child) {
            const QString text = child->text();
            if (childrenFetched.contains(text))
                continue;
            childrenFetched.insert(text);
            item->insertChild(child, sorted);
        }
    }

    return item;
}

static void insertSubItems(WatchItem *parent, const QVariantList &properties, const QVariant &refsVal)
{
    QTC_ASSERT(parent, return);
    foreach (const QVariant &property, properties) {
        QmlV8ObjectData propertyData = extractData(property, refsVal);
        auto item = new WatchItem;
        item->name = QString::fromUtf8(propertyData.name);

        // Check for v8 specific local data
        if (item->name.startsWith(QLatin1Char('.')) || item->name.isEmpty())
            continue;
        if (parent->type == "object") {
            if (parent->value == _("Array"))
                item->exp = parent->exp + '[' + item->name.toLatin1() + ']';
            else if (parent->value == _("Object"))
                item->exp = parent->exp + '.' + item->name.toLatin1();
        } else {
            item->exp = item->name.toLatin1();
        }

        item->iname = parent->iname + '.' + item->name.toLatin1();
        item->id = propertyData.handle;
        item->type = propertyData.type;
        item->value = propertyData.value.toString();
        item->setHasChildren(propertyData.properties.count());
        parent->appendChild(item);
    }
}

void QmlEnginePrivate::handleEvaluate(int sequence, bool success,
                                      const QVariant &bodyVal, const QVariant &refsVal)
{
    //    { "seq"         : <number>,
    //      "type"        : "response",
    //      "request_seq" : <number>,
    //      "command"     : "evaluate",
    //      "body"        : ...
    //      "running"     : <is the VM running after sending this response>
    //      "success"     : true
    //    }
    WatchHandler *watchHandler = engine->watchHandler();
    if (updateLocalsAndWatchers.contains(sequence)) {
        updateLocalsAndWatchers.removeOne(sequence);
        //Update the locals
        foreach (int index, currentFrameScopes)
            scope(index);
        //Also update "this"
        QByteArray iname("local.this");
        const WatchItem *parent = watchHandler->findItem(iname);
        localsAndWatchers.insertMulti(parent->id, iname);
        lookup(QList<int>() << parent->id);

    } else if (debuggerCommands.contains(sequence)) {
        updateLocalsAndWatchers.removeOne(sequence);
        QmlV8ObjectData body = extractData(bodyVal, refsVal);
        if (auto consoleManager = ConsoleManagerInterface::instance()) {
            if (ConsoleItem *item = constructLogItemTree(consoleManager->rootItem(), body, refsVal))
                consoleManager->printToConsolePane(item);
        }
        //Update the locals
        foreach (int index, currentFrameScopes)
            scope(index);

    } else {
        QmlV8ObjectData body = extractData(bodyVal, refsVal);
        if (evaluatingExpression.contains(sequence)) {
            QString exp =  evaluatingExpression.take(sequence);
            //Do we have request to evaluate a local?
            if (exp.startsWith(QLatin1String("local."))) {
                WatchItem *item = watchHandler->findItem(exp.toLatin1());
                insertSubItems(item, body.properties, refsVal);
            } else {
                QByteArray iname = watchHandler->watcherName(exp.toLatin1());
                SDEBUG(QString(iname));

                auto item = new WatchItem(iname, exp);
                item->exp = exp.toLatin1();
                item->id = body.handle;
                if (success) {
                    item->type = body.type;
                    item->value = body.value.toString();
                    item->wantsChildren = body.properties.count();
                } else {
                    //Do not set type since it is unknown
                    item->setError(body.value.toString());
                }
                watchHandler->insertItem(item);
                insertSubItems(item, body.properties, refsVal);
            }
            //Insert the newly evaluated expression to the Watchers Window
        }
    }
}

void QmlEnginePrivate::handleLookup(const QVariant &bodyVal, const QVariant &refsVal)
{
    //    { "seq"         : <number>,
    //      "type"        : "response",
    //      "request_seq" : <number>,
    //      "command"     : "lookup",
    //      "body"        : <array of serialized objects indexed using their handle>
    //      "running"     : <is the VM running after sending this response>
    //      "success"     : true
    //    }
    const QVariantMap body = bodyVal.toMap();

    QStringList handlesList = body.keys();
    WatchHandler *watchHandler = engine->watchHandler();
    foreach (const QString &handle, handlesList) {
        QmlV8ObjectData bodyObjectData = extractData(body.value(handle), refsVal);
        QByteArray prepend = localsAndWatchers.take(handle.toInt());

        if (prepend.startsWith("local.") || prepend.startsWith("watch.")) {
            // Data for expanded local/watch.
            // Could be an object or function.
            WatchItem *parent = watchHandler->findItem(prepend);
            insertSubItems(parent, bodyObjectData.properties, refsVal);
        } else {
            //rest
            auto item = new WatchItem;
            item->exp = prepend;
            item->name = QLatin1String(item->exp);
            item->iname = QByteArray("local.") + item->exp;
            item->id = handle.toInt();

            item->type = bodyObjectData.type;
            item->value = bodyObjectData.value.toString();

            item->setHasChildren(bodyObjectData.properties.count());

            engine->watchHandler()->insertItem(item);
        }
    }
    engine->watchHandler()->notifyUpdateFinished();
}

void QmlEnginePrivate::stateChanged(State state)
{
    engine->clientStateChanged(state);

    if (state == QmlDebugClient::Enabled) {
        /// Start session.
        flushSendBuffer();
        connect();
        //Query for the V8 version. This is
        //only for logging to the debuggerlog
        version();
    }
}

void QmlEnginePrivate::sendMessage(const QByteArray &msg)
{
    if (state() == Enabled)
        QmlDebugClient::sendMessage(msg);
    else
        sendBuffer.append(msg);
}

void QmlEnginePrivate::flushSendBuffer()
{
    QTC_ASSERT(state() == Enabled, return);
    foreach (const QByteArray &msg, sendBuffer)
       QmlDebugClient::sendMessage(msg);
    sendBuffer.clear();
}

DebuggerEngine *createQmlEngine(const DebuggerRunParameters &sp)
{
    return new QmlEngine(sp);
}

} // Internal
} // Debugger