#include "wizmainwindow.h"

#include <QtGui>

#ifdef Q_OS_MAC
#include <Carbon/Carbon.h>
#include "mac/wizmachelper.h"
#endif

#include "wizDocumentWebView.h"
#include "wizactions.h"
#include "wizaboutdialog.h"
#include "wizpreferencedialog.h"
#include "wizstatusbar.h"
#include "wizupgradenotifydialog.h"

#include "share/wizcommonui.h"
#include "share/wizui.h"
#include "share/wizmisc.h"
#include "share/wizuihelper.h"
#include "share/wizsettings.h"
#include "share/wizanimateaction.h"
#include "share/wizSearchIndexer.h"

#include "wizSearchWidget.h"

#include "wiznotestyle.h"
#include "wizdocumenthistory.h"

#include "wizButton.h"
#include "widgets/qsegmentcontrol.h"

#include "wizEditorToolBar.h"
#include "wizProgressDialog.h"
#include "wizDocumentSelectionView.h"
#include "share/wizGroupMessage.h"
#include "share/wizObjectDataDownloader.h"
#include "wizDocumentTransitionView.h"


MainWindow::MainWindow(CWizDatabaseManager& dbMgr, QWidget *parent)
    : QMainWindow(parent)
    , m_dbMgr(dbMgr)
    , m_progress(new CWizProgressDialog(this))
    , m_settings(new CWizUserSettings(dbMgr.db()))
    , m_sync(new CWizSyncThread(*this, this))
    , m_searchIndexer(new CWizSearchIndexer(m_dbMgr))
    , m_messageSync(new CWizGroupMessage(dbMgr.db()))
    , m_console(new CWizConsoleDialog(*this, this))
    , m_upgrade(new CWizUpgrade())
    , m_certManager(new CWizCertManager(*this))
    , m_cipherForm(new CWizUserCipherForm(*this, this))
    , m_groupAttribute(new CWizGroupAttributeForm(*this, this))
    , m_objectDownloadDialog(new CWizDownloadObjectDataDialog(dbMgr, this))
    , m_objectDownloaderHost(new CWizObjectDataDownloaderHost(dbMgr, this))
    , m_transitionView(new CWizDocumentTransitionView(this))
    #ifndef Q_OS_MAC
    , m_labelNotice(NULL)
    , m_optionsAction(NULL)
    #endif
    , m_toolBar(new QToolBar("Main", this))
    , m_menuBar(new QMenuBar(this))
    , m_statusBar(new CWizStatusBar(*this, this))
    , m_actions(new CWizActions(*this, this))
    , m_categoryPrivate(new CWizCategoryView(*this, this))
    , m_categoryTags(new CWizCategoryTagsView(*this, this))
    , m_categoryGroups(new CWizCategoryGroupsView(*this, this))
    , m_categoryLayer(new QWidget(this))
    , m_documents(new CWizDocumentListView(*this, this))
    , m_documentSelection(new CWizDocumentSelectionView(*this, this))
    , m_doc(new CWizDocumentView(*this, this))
    , m_history(new CWizDocumentViewHistory())
    , m_animateSync(new CWizAnimateAction(*this, this))
    , m_bRestart(false)
    , m_bLogoutRestart(false)
    , m_bUpdatingSelection(false)
    , m_bReadyQuit(false)
    , m_bRequestQuit(false)
{
    // FTS engine thread
    connect(m_searchIndexer, SIGNAL(documentFind(const WIZDOCUMENTDATAEX&)), \
            SLOT(on_searchDocumentFind(const WIZDOCUMENTDATAEX&)));

    QThread *threadFTS = new QThread();
    m_searchIndexer->moveToThread(threadFTS);
    threadFTS->start(QThread::LowestPriority);

    // upgrade check
    QThread *thread = new QThread();
    m_upgrade->moveToThread(thread);
    connect(m_upgrade, SIGNAL(checkFinished(bool)), SLOT(on_checkUpgrade_finished(bool)));
    thread->start();

    // upgrade check thread
#ifndef Q_OS_MAC
    // start update check thread
    //QTimer::singleShot(3 * 1000, m_upgrade, SLOT(checkUpgradeBegin()));
    //connect(m_upgrade, SIGNAL(finished()), SLOT(on_upgradeThread_finished()));
#endif // Q_OS_MAC

    // syncing thread
    m_syncTimer = new QTimer(this);
    connect(m_syncTimer, SIGNAL(timeout()), SLOT(on_actionSync_triggered()));
    if (m_settings->syncInterval() != -1) {
        QTimer::singleShot(3 * 1000, this, SLOT(on_actionSync_triggered()));
    }

    int nInterval = m_settings->syncInterval();
    if (nInterval == 0) {
        m_syncTimer->setInterval(15 * 60 * 1000);   // default 15 minutes
    } else {
        m_syncTimer->setInterval(nInterval * 60 * 1000);
    }

    // group messages sync thread
    QThread* threadMessage = new QThread();
    m_messageSync->moveToThread(threadMessage);
    threadMessage->start();

    // init GUI stuff
    m_category = m_categoryPrivate;

    connect(m_sync, SIGNAL(syncStarted()), SLOT(on_syncStarted()));
    connect(m_sync, SIGNAL(syncLogined()), SLOT(on_syncLogined()));
    connect(m_sync, SIGNAL(processLog(const QString&)), SLOT(on_syncProcessLog(const QString&)));
    connect(m_sync, SIGNAL(processErrorLog(const QString&)), SLOT(on_syncProcessErrorLog(const QString&)));
    connect(m_sync, SIGNAL(syncDone(bool)), SLOT(on_syncDone(bool)));

    initActions();
    initMenuBar();
    initToolBar();
    initClient();

    setWindowTitle(tr("WizNote"));

    restoreStatus();

    client()->hide();

#ifdef Q_WS_MAC
    setupFullScreenMode(this);
#endif
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);

    m_statusBar->adjustPosition();
}

void MainWindow::showEvent(QShowEvent* event)
{
    Q_UNUSED(event);

    m_statusBar->hide();
    m_cipherForm->hide();
    m_groupAttribute->hide();
}

bool MainWindow::requestThreadsQuit()
{
    bool bOk = true;
    if (m_sync->isRunning()) {
        m_sync->abort();
        m_sync->quit();
        bOk = false;
    }

    m_searchIndexer->abort();

//    if (m_searchIndexer->isRunning()) {
//        m_searchIndexer->worker()->abort();
//        m_searchIndexer->quit();
//        bOk = false;
//    }

    return bOk;
}

void MainWindow::on_quitTimeout()
{
    if (requestThreadsQuit()) {
        saveStatus();

        // FIXME : if document not valid will lead crash
        m_doc->web()->saveDocument(false);

        m_bReadyQuit = true;
        // back to closeEvent
        close();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // internal close event
    if (m_bRequestQuit) {
        if (m_bReadyQuit) {
            event->accept();
            return;
        } else {
            event->ignore();
            hide();
            m_timerQuit.start();
            return;
        }
    }

    // system close event
    if (isActiveWindow()) {
        // This is bug of Qt!
        // use hide() directly lead window can't be shown when click dock icon
        // call native API instead
#ifdef Q_OS_MAC
        ProcessSerialNumber pn;
        GetFrontProcess(&pn);
        ShowHideProcess(&pn,false);
#else
        // just quit on linux
        on_actionExit_triggered();
#endif
    } else {
        on_actionExit_triggered();
    }

    event->ignore();
}

void MainWindow::on_actionExit_triggered()
{
    m_bRequestQuit = true;

    m_timerQuit.setInterval(100);
    connect(&m_timerQuit, SIGNAL(timeout()), SLOT(on_quitTimeout()));
    m_timerQuit.start();
}

void MainWindow::on_checkUpgrade_finished(bool bUpgradeAvaliable)
{
    if (!bUpgradeAvaliable)
        return;

    QString strUrl = m_upgrade->getWhatsNewUrl();
    CWizUpgradeNotifyDialog notifyDialog(strUrl, this);
    if (QDialog::Accepted == notifyDialog.exec()) {
#if defined(Q_OS_MAC)
        QUrl url("http://www.wiz.cn/wiznote-mac.html");
#elif defined(Q_OS_LINUX)
        QUrl url("http://www.wiz.cn/wiznote-linux.html");
#else
        Q_ASSERT(0);
#endif
        QDesktopServices::openUrl(url);
    }
}

#ifdef WIZ_OBOSOLETE
void MainWindow::on_upgradeThread_finished()
{
    QString strUrl = m_upgrade->whatsNewUrl();
    if (strUrl.isEmpty()) {
        return;
    }

    CWizUpgradeNotifyDialog notifyDialog(strUrl, this);

    if (QDialog::Accepted == notifyDialog.exec()) {
        QFile file(::WizGetUpgradePath() + "WIZNOTE_READY_FOR_UPGRADE");
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.close();

        // restart immediately
        m_bRestart = true;
        on_actionExit_triggered();
    } else {
        // skip for this session
        QFile file(::WizGetUpgradePath() + "WIZNOTE_SKIP_THIS_SESSION");
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.close();
    }
}
#endif

MainWindow::~MainWindow()
{
    delete m_history;
}

void MainWindow::saveStatus()
{
    CWizSettings settings(WizGetSettingsFileName());
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/splitter", m_splitter->saveState());
}

void MainWindow::restoreStatus()
{
    CWizSettings settings(WizGetSettingsFileName());
    QByteArray geometry = settings.value("window/geometry").toByteArray();

    // main window
    if (geometry.isEmpty()) {
        setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, \
                                        sizeHint(), qApp->desktop()->availableGeometry()
                                        ));
    } else {
        restoreGeometry(geometry);
    }

    m_splitter->restoreState(settings.value("window/splitter").toByteArray());
}

void MainWindow::initActions()
{
    m_actions->init();
    m_animateSync->setAction(m_actions->actionFromName(WIZACTION_GLOBAL_SYNC));
    m_animateSync->setIcons("sync");
}

void MainWindow::initMenuBar()
{
    setMenuBar(m_menuBar);
    m_actions->buildMenuBar(m_menuBar, ::WizGetResourcesPath() + "files/mainmenu.ini");
}

void MainWindow::initToolBar()
{

#ifdef Q_OS_MAC
    m_toolBar->setIconSize(QSize(24, 24));
    setUnifiedTitleAndToolBarOnMac(true);
#else
    m_toolBar->setIconSize(QSize(32, 32));
#endif

    m_toolBar->setMovable(false);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    m_toolBar->addWidget(new CWizFixedSpacer(QSize(40, 1), m_toolBar));

    CWizButton* buttonSync = new CWizButton(*this, m_toolBar);
    buttonSync->setAction(m_actions->actionFromName(WIZACTION_GLOBAL_SYNC));
    m_toolBar->addWidget(buttonSync);

    m_toolBar->addWidget(new CWizFixedSpacer(QSize(20, 1), m_toolBar));
    CWizButton* buttonNew = new CWizButton(*this, m_toolBar);
    buttonNew->setAction(m_actions->actionFromName(WIZACTION_GLOBAL_NEW_DOCUMENT));
    m_toolBar->addWidget(buttonNew);

    m_toolBar->addWidget(new CWizFixedSpacer(QSize(20, 1), m_toolBar));
    CWizButton* btnViewMessages = new CWizButton(*this, m_toolBar);
    btnViewMessages->setAction(m_actions->actionFromName(WIZACTION_GLOBAL_VIEW_MESSAGES));
    m_toolBar->addWidget(btnViewMessages);

    m_toolBar->addWidget(new CWizSpacer(m_toolBar));
    m_searchBox = new CWizSearchBox(*this, this);
    connect(m_searchBox, SIGNAL(doSearch(const QString&)), SLOT(on_search_doSearch(const QString&)));

    m_toolBar->addWidget(m_searchBox);
    m_toolBar->layout()->setAlignment(m_searchBox, Qt::AlignBottom);

    m_toolBar->addWidget(new CWizFixedSpacer(QSize(20, 1), m_toolBar));

    addToolBar(m_toolBar);
}

QWidget* MainWindow::setupCategorySwitchButtons()
{
    QWidget* categorySwitchWidget = new QWidget(this);

    m_categorySwitchSegmentBtn = new QtSegmentControl(categorySwitchWidget);
    m_categorySwitchSegmentBtn->setSelectionBehavior(QtSegmentControl::SelectOne);
    m_categorySwitchSegmentBtn->setCount(3);
    m_categorySwitchSegmentBtn->setIconSize(QSize(18, 18));
    m_categorySwitchSegmentBtn->setSegmentIcon(0, ::WizLoadSkinIcon3("folder", QIcon::Normal));
    m_categorySwitchSegmentBtn->setSegmentIcon(1, ::WizLoadSkinIcon3("tag", QIcon::Normal));
    m_categorySwitchSegmentBtn->setSegmentIcon(2, ::WizLoadSkinIcon3("groups", QIcon::Normal));
    m_categorySwitchSegmentBtn->setSegmentSelected(0, true);
    connect(m_categorySwitchSegmentBtn, SIGNAL(segmentSelected(int)), this, SLOT(on_actionCategorySwitch_triggered(int)));

    QHBoxLayout* layoutCategorySwitch = new QHBoxLayout();
    categorySwitchWidget->setLayout(layoutCategorySwitch);
    layoutCategorySwitch->setContentsMargins(5, 0, 5, 0);
    layoutCategorySwitch->addStretch();
    layoutCategorySwitch->addWidget(m_categorySwitchSegmentBtn);
    layoutCategorySwitch->addStretch();

    return categorySwitchWidget;
}

void MainWindow::initClient()
{
    QWidget* client = new QWidget(this);
    setCentralWidget(client);

    client->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    QPalette pal = client->palette();
    pal.setBrush(QPalette::Window, WizGetLeftViewBrush());
    client->setPalette(pal);
    client->setAutoFillBackground(true);

    QHBoxLayout* layout = new QHBoxLayout(client);
    layout->setContentsMargins(0, 0, 0, 0);
    client->setLayout(layout);

#ifndef Q_OS_MAC
    WizInitWidgetMargins(m_settings->skin(), client, "Client");
    WizInitWidgetMargins(m_settings->skin(), m_doc, "Document");
#endif

    m_splitter = new CWizSplitter(client);
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter);

    QWidget* categoryPanel = new QWidget(m_splitter);
    categoryPanel->setMinimumWidth(30);
    QVBoxLayout* layoutCategoryPanel = new QVBoxLayout(categoryPanel);
    categoryPanel->setLayout(layoutCategoryPanel);
    layoutCategoryPanel->setContentsMargins(0, 0, 0, 0);
    layoutCategoryPanel->setSpacing(0);
    layoutCategoryPanel->addWidget(setupCategorySwitchButtons());
    layoutCategoryPanel->addWidget(m_categoryLayer);

    QHBoxLayout* layoutCategories = new QHBoxLayout(m_categoryLayer);
    m_categoryLayer->setLayout(layoutCategories);
    layoutCategories->setContentsMargins(0, 0, 0, 0);
    layoutCategories->setSpacing(0);

    layoutCategories->addWidget(m_categoryPrivate);
    layoutCategories->addWidget(m_categoryTags);
    layoutCategories->addWidget(m_categoryGroups);
    m_categoryTags->hide();
    m_categoryGroups->hide();

    QWidget* documentPanel = new QWidget(m_splitter);
    QHBoxLayout* layoutDocument = new QHBoxLayout();
    layoutDocument->setContentsMargins(0, 0, 0, 0);
    layoutDocument->setSpacing(0);
    documentPanel->setLayout(layoutDocument);
    layoutDocument->addWidget(m_doc);
    layoutDocument->addWidget(m_documentSelection);
    m_documentSelection->hide();
    // append after client
    m_doc->layout()->addWidget(m_transitionView);
    //layoutDocument->addWidget(m_transitionView);
    m_transitionView->hide();

    m_splitter->addWidget(categoryPanel);
    m_splitter->addWidget(m_documents);
    m_splitter->addWidget(documentPanel);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);


#ifndef Q_OS_MAC
    //connect(splitter, SIGNAL(splitterMoved(int,int)), this, SLOT(on_client_splitterMoved(int, int)));
#endif
}

//void MainWindow::resetNotice()
//{
//#ifdef Q_OS_MAC
//#else
//    if (!m_labelNotice)
//        return;
//    //
//    m_labelNotice->setText("");
//    //
//    QString notice;
//    if (!::WizGetNotice(notice)
//        || notice.isEmpty())
//        return;
//    //
//    CString strTitle = ::WizGetCommandLineValue(notice, "Title");
//    CString strLink = ::WizGetCommandLineValue(notice, "Link");
//    //
//    CString strText = CString("<a href=\"%1\">%2</a>").arg(strLink, strTitle);
//    //
//    m_labelNotice->setText(strText);
//#endif
//}

void MainWindow::init()
{
    connect(m_categoryPrivate, SIGNAL(itemSelectionChanged()), SLOT(on_category_itemSelectionChanged()));
    connect(m_categoryTags, SIGNAL(itemSelectionChanged()), SLOT(on_category_itemSelectionChanged()));
    connect(m_categoryGroups, SIGNAL(itemSelectionChanged()), SLOT(on_category_itemSelectionChanged()));

    connect(m_categoryPrivate, SIGNAL(newDocument()), SLOT(on_actionNewNote_triggered()));
    connect(m_documents, SIGNAL(itemSelectionChanged()), SLOT(on_documents_itemSelectionChanged()));

    m_categoryPrivate->baseInit();
    m_categoryTags->baseInit();
    m_categoryGroups->baseInit();
}

void MainWindow::on_syncStarted()
{
    m_animateSync->startPlay();
    m_syncTimer->stop();
}

void MainWindow::on_syncLogined()
{
    //m_sync.userInfo().strNotice;
    //::WizSetNotice(m_sync.userInfo().strNotice);
    //
#ifdef QT_DEBUG
    //::WizSetNotice("/Title=test /Link=http://www.wiz.cn/");
#endif
    //resetNotice();
}

void MainWindow::on_syncDone(bool error)
{
    Q_UNUSED(error);

    m_statusBar->hide();
    m_animateSync->stopPlay();

    if (m_settings->syncInterval() != -1) {
        m_syncTimer->start();
    }
}

void MainWindow::on_syncProcessLog(const QString& msg)
{
    TOLOG(msg);

    QString strMsg;
    if (msg.length() > 50) {
        strMsg = msg.left(50) + "..";
    } else {
        strMsg = msg;
    }

    m_statusBar->showText(strMsg);
}

void MainWindow::on_syncProcessDebugLog(const QString& strMsg)
{
    DEBUG_TOLOG(strMsg);
}

void MainWindow::on_syncProcessErrorLog(const QString& strMsg)
{
    TOLOG(strMsg);
}

void MainWindow::on_actionSync_triggered()
{
    m_certManager->downloadUserCert();
    m_sync->startSyncing();
}

void MainWindow::on_actionNewNote_triggered()
{
    WIZDOCUMENTDATA data;

    // private category is seletected
    if (m_category == m_categoryPrivate) {
        CWizFolder* pFolder = m_categoryPrivate->SelectedFolder();
        if (!pFolder) {
            m_categoryPrivate->addAndSelectFolder("/My Notes/");
            pFolder = m_categoryPrivate->SelectedFolder();

            if (!pFolder)
                return;
        }

        //m_dbMgr.db().CreateDocumentAndInit("<body><div>&nbsp;</div></body>", "", 0, tr("New note"), "newnote", pFolder->Location(), "", data);
        bool ret = m_dbMgr.db().CreateDocumentAndInit("", "", 0, tr("New note"), "newnote", pFolder->Location(), "", data);
        if (!ret) {
            TOLOG("Create new document failed!");
            delete pFolder;
            return;
        }

        delete pFolder;

    // if tag category is selected, create document at default location and also assign this tag to new document
    } else if (m_category == m_categoryTags) {
        bool ret = m_dbMgr.db().CreateDocumentAndInit("", "", 0, tr("New note"), "newnote", "/My Notes/", "", data);
        if (!ret) {
            TOLOG("Create new document failed!");
            return;
        }

        if (CWizCategoryViewTagItem* p = dynamic_cast<CWizCategoryViewTagItem*>(m_categoryTags->currentItem())) {
            CWizDocument doc(m_dbMgr.db(), data);
            doc.AddTag(p->tag());
        } else {
            m_categoryTags->setCurrentItem(m_categoryTags->findAllTags());
        }

    // if group category is selected
    } else if (m_category == m_categoryGroups) {
        QString strKbGUID;
        WIZTAGDATA tag;
        // if group root selected
        if (CWizCategoryViewGroupRootItem* pRoot = dynamic_cast<CWizCategoryViewGroupRootItem*>(m_categoryGroups->currentItem())) {
            strKbGUID = pRoot->kbGUID();
        } else if (CWizCategoryViewGroupItem* p = dynamic_cast<CWizCategoryViewGroupItem*>(m_categoryGroups->currentItem())) {
            strKbGUID = p->kbGUID();
            tag = p->tag();
        } else {
            Q_ASSERT(0);
            return;
        }

        // FIXME: root of all groups will be selected, fix this issue
        if (strKbGUID.isEmpty()) {
            return;
        }

        CWizDatabase& db = m_dbMgr.db(strKbGUID);
        bool ret = db.CreateDocumentAndInit("", "", 0, tr("New note"), "newnote", "/My Notes/", "", data);
        if (!ret) {
            TOLOG("Create new document for group failed!");
            return;
        }

        // also assign tag(folder)
        if (!tag.strGUID.isEmpty()) {
            CWizDocument doc(db, data);
            doc.AddTag(tag);
        }

    } else {
        Q_ASSERT(0);
    }

    m_documentForEditing = data;
    m_documents->addAndSelectDocument(data);
}

void MainWindow::on_actionViewMessages_triggered()
{
    // read messages from db
    CWizMessageDataArray arrayMsg;
    m_dbMgr.db().getAllMessages(arrayMsg);

    if (!arrayMsg.size())
        return;

    // set documents
    m_documents->setDocuments(arrayMsg);
}

void MainWindow::on_actionEditingUndo_triggered()
{
    m_doc->web()->editorCommandExecuteUndo();
}

void MainWindow::on_actionEditingRedo_triggered()
{
    m_doc->web()->editorCommandExecuteRedo();
}

void MainWindow::on_actionViewToggleCategory_triggered()
{
    QWidget* category = m_splitter->widget(0);
    if (category->isVisible()) {
        category->hide();
    } else {
        category->show();
    }

    m_actions->toggleActionText(WIZACTION_GLOBAL_TOGGLE_CATEGORY);
}

void MainWindow::on_actionViewToggleFullscreen_triggered()
{
#ifdef Q_WS_MAC
    toggleFullScreenMode(this);
    m_actions->toggleActionText(WIZACTION_GLOBAL_TOGGLE_FULLSCREEN);
#endif
}

void MainWindow::on_actionFormatJustifyLeft_triggered()
{
    m_doc->web()->editorCommandExecuteJustifyLeft();
}

void MainWindow::on_actionFormatJustifyRight_triggered()
{
    m_doc->web()->editorCommandExecuteJustifyRight();
}

void MainWindow::on_actionFormatJustifyCenter_triggered()
{
    m_doc->web()->editorCommandExecuteJustifyCenter();
}

void MainWindow::on_actionFormatJustifyJustify_triggered()
{
    m_doc->web()->editorCommandExecuteJustifyJustify();
}

void MainWindow::on_actionFormatInsertOrderedList_triggered()
{
    m_doc->web()->editorCommandExecuteInsertOrderedList();
}

void MainWindow::on_actionFormatInsertUnorderedList_triggered()
{
    m_doc->web()->editorCommandExecuteInsertUnorderedList();
}

void MainWindow::on_actionFormatInsertTable_triggered()
{
    m_doc->web()->editorCommandExecuteTableInsert();
}

void MainWindow::on_actionFormatInsertLink_triggered()
{
    m_doc->web()->editorCommandExecuteLinkInsert();
}

void MainWindow::on_actionFormatForeColor_triggered()
{
    m_doc->web()->editorCommandExecuteForeColor();
}

void MainWindow::on_actionFormatBold_triggered()
{
    m_doc->web()->editorCommandExecuteBold();
}

void MainWindow::on_actionFormatItalic_triggered()
{
    m_doc->web()->editorCommandExecuteItalic();
}

void MainWindow::on_actionFormatUnderLine_triggered()
{
    m_doc->web()->editorCommandExecuteUnderLine();
}

void MainWindow::on_actionFormatStrikeThrough_triggered()
{
    m_doc->web()->editorCommandExecuteStrikeThrough();
}

void MainWindow::on_actionFormatInsertHorizontal_triggered()
{
    m_doc->web()->editorCommandExecuteInsertHorizontal();
}

void MainWindow::on_actionFormatInsertDate_triggered()
{
    m_doc->web()->editorCommandExecuteInsertDate();
}

void MainWindow::on_actionFormatInsertTime_triggered()
{
    m_doc->web()->editorCommandExecuteInsertTime();
}

void MainWindow::on_actionFormatIndent_triggered()
{
    m_doc->web()->editorCommandExecuteIndent();
}

void MainWindow::on_actionFormatOutdent_triggered()
{
    m_doc->web()->editorCommandExecuteOutdent();
}

void MainWindow::on_actionFormatRemoveFormat_triggered()
{
    m_doc->web()->editorCommandExecuteRemoveFormat();
}

void MainWindow::on_actionEditorViewSource_triggered()
{
    m_doc->web()->editorCommandExecuteViewSource();
}

//void MainWindow::on_actionDeleteCurrentNote_triggered()
//{
//    WIZDOCUMENTDATA document = m_doc->document();
//    if (document.strGUID.IsEmpty())
//        return;
//
//    if (!m_dbMgr.isPrivate(document.strKbGUID)) {
//        return;
//    }
//
//    CWizDocument doc(m_dbMgr.db(), document);
//    doc.Delete();
//}

void MainWindow::on_actionConsole_triggered()
{
    m_console->show();
}

void MainWindow::on_actionLogout_triggered()
{
    // save state
    m_settings->setAutoLogin(false);
    m_settings->setPassword();
    m_bLogoutRestart = true;
    on_actionExit_triggered();
}

void MainWindow::on_actionAbout_triggered()
{
    AboutDialog dlg(*this, this);
    dlg.exec();
}

void MainWindow::on_actionPreference_triggered()
{
    CWizPreferenceWindow preference(*this, this);

    connect(&preference, SIGNAL(settingsChanged(WizOptionsType)), SLOT(on_options_settingsChanged(WizOptionsType)));
    connect(&preference, SIGNAL(restartForSettings()), SLOT(on_options_restartForSettings()));
    preference.exec();
}

void MainWindow::on_actionRebuildFTS_triggered()
{
    // FIXME: ensure aborted before rebuild
    m_searchIndexer->rebuild();

//    if (!QMetaObject::invokeMethod(m_searchIndexer->worker(), "rebuildFTSIndex")) {
//        TOLOG("thread call to rebuild FTS failed");
//    }
}

void MainWindow::on_actionSearch_triggered()
{
    m_searchBox->focus();
}

void MainWindow::on_actionResetSearch_triggered()
{
    m_searchBox->clear();
    m_searchBox->focus();
    m_category->restoreSelection();
}

//void MainWindow::on_searchIndexerStarted()
//{
//    QTimer::singleShot(5 * 1000, m_searchIndexer->worker(), SLOT(buildFTSIndex()));
//}

void MainWindow::on_searchDocumentFind(const WIZDOCUMENTDATAEX& doc)
{
    m_documents->addDocument(doc, true);
    on_documents_itemSelectionChanged();
}

void MainWindow::on_search_doSearch(const QString& keywords)
{
    if (keywords.isEmpty()) {
        on_actionResetSearch_triggered();
        return;
    }

    m_category->saveSelection();
    m_documents->clear();

    m_searchIndexer->search(keywords, 1000); // FIXME

//    if (!QMetaObject::invokeMethod(m_searchIndexer->worker(), "search", \
//                                   Q_ARG(QString, keywords), \
//                                   Q_ARG(int, 100))) {
//        TOLOG("FTS search failed");
//    }
}

#ifndef Q_OS_MAC
void MainWindow::on_actionPopupMainMenu_triggered()
{
    QAction* pAction = m_actions->actionFromName("actionPopupMainMenu");
    QRect rc = m_toolBar->actionGeometry(pAction);
    QPoint pt = m_toolBar->mapToGlobal(QPoint(rc.left(), rc.bottom()));

    CWizSettings settings(::WizGetResourcesPath() + "files/mainmenu.ini");

    QMenu* pMenu = new QMenu(this);
    m_actions->buildMenu(pMenu, settings, pAction->objectName());

    pMenu->popup(pt);
}

void MainWindow::on_client_splitterMoved(int pos, int index)
{
    if (0 == index)
    {
    }
    else if (1 == index)
    {
        QPoint pt(pos, 0);
        //
        pt = m_splitter->mapToGlobal(pt);
        //
        adjustToolBarSpacerToPos(0, pt.x());
    }
    else if (2 == index)
    {
        QPoint pt(pos, 0);

        pt = m_splitter->mapToGlobal(pt);

        adjustToolBarSpacerToPos(1, pt.x());
    }
}

#endif

void MainWindow::on_actionGoBack_triggered()
{
    if (!m_history->canBack())
        return;

    WIZDOCUMENTDATA data = m_history->back();
    viewDocument(data, false);
    locateDocument(data);
}

void MainWindow::on_actionGoForward_triggered()
{
    if (!m_history->canForward())
        return;

    WIZDOCUMENTDATA data = m_history->forward();
    viewDocument(data, false);
    locateDocument(data);
}

void MainWindow::on_actionCategorySwitch_triggered(int index)
{
    QAction* action;
    if (index == 0) {
        action = m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_PRIVATE);
        if (action->isEnabled())
            action->trigger();
    } else if (index == 1) {
        action = m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_TAGS);
        if (action->isEnabled())
            action->trigger();
    } else if (index == 2) {
        action = m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_GROUPS);
        if (action->isEnabled())
            action->trigger();
    } else {
        Q_ASSERT(0);
    }
}

void MainWindow::on_actionCategorySwitchPrivate_triggered()
{
    if (m_category == m_categoryPrivate) {
        return;
    }

    categorySwitchTo(m_category, m_categoryPrivate);
}

void MainWindow::on_actionCategorySwitchTags_triggered()
{
    if (m_category == m_categoryTags) {
        return;
    }

    categorySwitchTo(m_category, m_categoryTags);
}

void MainWindow::on_actionCategorySwitchGroups_triggered()
{
    if (m_category == m_categoryGroups) {
        return;
    }

    categorySwitchTo(m_category, m_categoryGroups);
}

void MainWindow::categorySwitchTo(CWizCategoryBaseView* sourceCategory, CWizCategoryBaseView* destCategory)
{
    if (!sourceCategory->isVisible()) {
        return;
    }

    sourceCategory->saveSelection();
    destCategory->restoreSelection();

    m_categorySwitchSegmentBtn->setEnabled(false);
    m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_PRIVATE)->setEnabled(false);
    m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_TAGS)->setEnabled(false);
    m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_GROUPS)->setEnabled(false);

    QRect rectOld(m_categoryLayer->geometry());

    // break layout and do animation
    QHBoxLayout* layout = qobject_cast<QHBoxLayout *>(m_categoryLayer->layout());
    layout->removeWidget(sourceCategory);
    destCategory->lower();
    destCategory->show();
    m_category = destCategory;

    QPropertyAnimation* anim1 = new QPropertyAnimation(sourceCategory, "geometry");
    anim1->setEasingCurve(QEasingCurve::InOutExpo);
    anim1->setDuration(200);
    anim1->setEndValue(QRect(rectOld.width(), 0, 0, rectOld.height()));
    connect(anim1, SIGNAL(stateChanged(QAbstractAnimation::State, QAbstractAnimation::State)), \
            SLOT(onAnimationCategorySwitchStateChanged(QAbstractAnimation::State, QAbstractAnimation::State)));

    anim1->start();
}

void MainWindow::onAnimationCategorySwitchStateChanged(QAbstractAnimation::State newState, QAbstractAnimation::State oldState)
{
    if (newState == QAbstractAnimation::Stopped && oldState == QAbstractAnimation::Running) {
        QPropertyAnimation* animate = qobject_cast<QPropertyAnimation *>(sender());
        CWizCategoryBaseView* category = qobject_cast<CWizCategoryBaseView *>(animate->targetObject());
        category->hide();

        QHBoxLayout* layout = qobject_cast<QHBoxLayout *>(m_categoryLayer->layout());
        layout->addWidget(category);

        m_categoryLayer->update();

        m_categorySwitchSegmentBtn->setEnabled(true);
        m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_PRIVATE)->setEnabled(true);
        m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_TAGS)->setEnabled(true);
        m_actions->actionFromName(WIZACTION_SWITCH_CATEGORY_GROUPS)->setEnabled(true);
    }
}

void MainWindow::on_category_itemSelectionChanged()
{
    CWizDocumentDataArray arrayDocument;

    CWizCategoryBaseView* category = qobject_cast<CWizCategoryBaseView *>(sender());
    if (!category)
        return;

    QString kbGUID = category->selectedItemKbGUID();
    if (!kbGUID.isEmpty()) {
        resetPermission(kbGUID, "");
    }

    category->getDocuments(arrayDocument);
    m_documents->setDocuments(arrayDocument);

    if (arrayDocument.empty()) {
        on_documents_itemSelectionChanged();
    }
}

void MainWindow::on_documents_itemSelectionChanged()
{
    // hide other form
    m_cipherForm->hide();
    m_groupAttribute->sheetHide();

    CWizDocumentDataArray arrayDocument;
    m_documents->getSelectedDocuments(arrayDocument);

    if (arrayDocument.size() == 1) {
        //if (!m_doc->isVisible()) {
        //    m_doc->show();
        //    m_documentSelection->hide();
        //    //m_doc->resize(m_documentSelection->size());
        //}

        if (!m_bUpdatingSelection) {
            viewDocument(arrayDocument[0], true);
        }
    } else if (arrayDocument.size() > 1) {
        //if (!m_documentSelection->isVisible()) {
        //    m_documentSelection->show();
        //    m_doc->hide();
        //    //m_documentSelection->resize(m_doc->size());
        //}
//
        //m_documentSelection->requestDocuments(arrayDocument);
    }
}

void MainWindow::on_options_settingsChanged(WizOptionsType type)
{
    if (wizoptionsNoteView == type) {
        m_doc->settingsChanged();
    } else if (wizoptionsSync == type) {

        int nInterval = m_settings->syncInterval();
        if (nInterval == -1) {
            m_syncTimer->stop();
        } else {
            nInterval = nInterval < 5 ? 5 : nInterval;
            m_syncTimer->setInterval(nInterval * 60 * 1000);
        }

        m_sync->resetProxy();
    } else if (wizoptionsFont == type) {
        m_doc->web()->editorResetFont();
    }
}

void MainWindow::on_options_restartForSettings()
{
    m_bRestart = true;
    on_actionExit_triggered();
}

void MainWindow::resetPermission(const QString& strKbGUID, const QString& strOwner)
{
    Q_ASSERT(!strKbGUID.isEmpty());

    int nPerm = m_dbMgr.db(strKbGUID).permission();
    bool isGroup = m_dbMgr.db().kbGUID() != strKbGUID;

    // Admin, Super, do anything
    if (nPerm == WIZ_USERGROUP_ADMIN || nPerm == WIZ_USERGROUP_SUPER) {
        // enable editing
        m_doc->setReadOnly(false, isGroup);

        // enable create tag

        // enable new document
        m_actions->actionFromName(WIZACTION_GLOBAL_NEW_DOCUMENT)->setEnabled(true);

        // enable delete document
        //m_actions->actionFromName("actionDeleteCurrentNote")->setEnabled(true);

    // Editor, only disable create tag
    } else if (nPerm == WIZ_USERGROUP_EDITOR) {
        m_doc->setReadOnly(false, isGroup);
        m_actions->actionFromName(WIZACTION_GLOBAL_NEW_DOCUMENT)->setEnabled(true);
        //m_actions->actionFromName("actionDeleteCurrentNote")->setEnabled(true);

    // Author
    } else if (nPerm == WIZ_USERGROUP_AUTHOR) {
        m_actions->actionFromName(WIZACTION_GLOBAL_NEW_DOCUMENT)->setEnabled(true);

        // author is owner
        QString strUserId = m_dbMgr.db().getUserId();
        if (strOwner == strUserId) {
            m_doc->setReadOnly(false, isGroup);
            //m_actions->actionFromName("actionDeleteCurrentNote")->setEnabled(true);

        // not owner
        } else {
            m_doc->setReadOnly(true, isGroup);
            //m_actions->actionFromName("actionDeleteCurrentNote")->setEnabled(false);
        }

    // reader
    } else if (nPerm == WIZ_USERGROUP_READER) {
        m_doc->setReadOnly(true, isGroup);
        m_actions->actionFromName(WIZACTION_GLOBAL_NEW_DOCUMENT)->setEnabled(false);
        //m_actions->actionFromName("actionDeleteCurrentNote")->setEnabled(false);
    } else {
        Q_ASSERT(0);
    }
}

void MainWindow::viewDocument(const WIZDOCUMENTDATA& data, bool addToHistory)
{
    Q_ASSERT(!data.strKbGUID.isEmpty());

    CWizDocument* doc = new CWizDocument(m_dbMgr.db(data.strKbGUID), data);

    if (doc->GUID() == m_doc->document().strGUID)
        return;

    bool forceEdit = false;
    if (doc->GUID() == m_documentForEditing.strGUID) {
        m_documentForEditing = WIZDOCUMENTDATA();
        forceEdit = true;
    }

    resetPermission(data.strKbGUID, data.strOwner);

    if (!m_doc->viewDocument(data, forceEdit))
        return;

    if (addToHistory) {
        m_history->addHistory(data);
    }

    //m_actions->actionFromName("actionGoBack")->setEnabled(m_history->canBack());
    //m_actions->actionFromName("actionGoForward")->setEnabled(m_history->canForward());
}

void MainWindow::locateDocument(const WIZDOCUMENTDATA& data)
{
    try
    {
        m_bUpdatingSelection = true;
        m_categoryPrivate->addAndSelectFolder(data.strLocation);
        m_documents->addAndSelectDocument(data);
    }
    catch (...)
    {

    }

    m_bUpdatingSelection = false;
}

#ifndef Q_OS_MAC
CWizFixedSpacer* MainWindow::findFixedSpacer(int index)
{
    if (!m_toolBar)
        return NULL;
    //
    int i = 0;
    //
    QList<QAction*> actions = m_toolBar->actions();
    foreach (QAction* action, actions)
    {
        QWidget* widget = m_toolBar->widgetForAction(action);
        if (!widget)
            continue;
        //
        if (CWizFixedSpacer* spacer = dynamic_cast<CWizFixedSpacer*>(widget))
        {
            if (index == i)
                return spacer;
            //
            i++;
        }
    }
    //
    return NULL;
}


void MainWindow::adjustToolBarSpacerToPos(int index, int pos)
{
    if (!m_toolBar)
        return;
    //
    CWizFixedSpacer* spacer = findFixedSpacer(index);
    if (!spacer)
        return;
    //
    QPoint pt = spacer->mapToGlobal(QPoint(0, 0));
    //
    if (pt.x() > pos)
        return;
    //
    int width = pos - pt.x();
    //
    spacer->adjustWidth(width);
}


#endif

//QObject* MainWindow::CategoryCtrl()
//{
//    return m_category;
//}
//
//QObject* MainWindow::DocumentsCtrl()
//{
//    return m_documents;
//}


//================================================================================
// WizExplorerApp APIs
//================================================================================

QObject* MainWindow::DocumentsCtrl()
{
    return m_documents;
}

QObject* MainWindow::CreateWizObject(const QString& strObjectID)
{
    CString str(strObjectID);
    if (0 == str.CompareNoCase("WizKMControls.WizCommonUI"))
    {
        static CWizCommonUI* commonUI = new CWizCommonUI(this);
        return commonUI;
    }

    return NULL;
}

void MainWindow::SetDocumentModified(bool modified)
{
    m_doc->setModified(modified);
}

void MainWindow::ResetInitialStyle()
{
    m_doc->web()->initEditorStyle();
}

void MainWindow::ResetEditorToolBar()
{
    m_doc->editorToolBar()->resetToolbar();
}

void MainWindow::ResetContextMenuAndPop(const QPoint& pos)
{
    m_doc->editorToolBar()->resetContextMenuAndPop(pos);
}

void MainWindow::SetSavingDocument(bool saving)
{
    //m_statusBar->setVisible(saving);
    if (saving) {
        //m_statusBar->setVisible(true);
        m_statusBar->showText(tr("Saving note..."));
        //qApp->processEvents(QEventLoop::AllEvents);
    } else {
        m_statusBar->hide();
    }
}

void MainWindow::ProcessClipboardBeforePaste(const QVariantMap& data)
{
    Q_UNUSED(data);
    // QVariantMap =  {html: text, textContent: text};
    //qDebug() << data.value("html").toString();
    //qDebug() << data.value("textContent").toString();

    QClipboard* clipboard = QApplication::clipboard();
    QImage image = clipboard->image();
    if (!image.isNull()) {
        qDebug() << "clipboard with image";
        // save clipboard image to $TMPDIR
        QString strTempPath = WizGlobal()->GetTempPath();
        CString strFileName = strTempPath + WizIntToStr(GetTickCount()) + ".png";
        if (!image.save(strFileName)) {
            TOLOG("ERROR: Can't save clipboard image to file");
            return;
        }

        QString strHtml = QString("<img border=\"0\" src=\"file://%1\" />").arg(strFileName);
        web()->editorCommandExecuteInsertHtml(strHtml, true);
        return;
    }
}
