#include "wiznoteinfoform.h"
#include "ui_wiznoteinfoform.h"

#include <QFile>

CWizNoteInfoForm::CWizNoteInfoForm(CWizDatabaseManager& db, QWidget *parent)
    : CWizPopupWidget(parent)
    , m_dbMgr(db)
    , ui(new Ui::CWizNoteInfoForm)
{
    ui->setupUi(this);
    setContentsMargins(8, 20, 8, 8);

    ui->editTitle->setReadOnly(true);
    ui->editCreateTime->setReadOnly(true);
    ui->editUpdateTime->setReadOnly(true);
    ui->editURL->setReadOnly(true);
    ui->editAuthor->setReadOnly(true);
    ui->checkEncrypted->setEnabled(false);
}

CWizNoteInfoForm::~CWizNoteInfoForm()
{
    delete ui;
}

QSize CWizNoteInfoForm::sizeHint() const
{
    return QSize(420, 350);
}

void CWizNoteInfoForm::setDocument(const WIZDOCUMENTDATA& data)
{
    Q_ASSERT(!data.strKbGUID.isEmpty());

    CWizDatabase& db = m_dbMgr.db(data.strKbGUID);
    QString doc = db.GetDocumentFileName(data.strGUID);
    QString sz = ::WizGetFileSizeHumanReadalbe(doc);

    ui->editTitle->setText(data.strTitle);

    // private document
    if (data.strKbGUID == m_dbMgr.db().kbGUID()) {
        ui->labelNotebook->setText(data.strLocation);

        QString tags = db.GetDocumentTagsText(data.strGUID);
        ui->labelTags->setText(tags);

        ui->editAuthor->setText(data.strAuthor);

    // group document
    } else {
        CWizTagDataArray arrayTag;
        if (!db.GetDocumentTags(data.strGUID, arrayTag)) {
            ui->labelNotebook->clear();
        } else {
            if (arrayTag.size() > 1) {
                TOLOG1("Group document should only have one tag: %1", data.strTitle);
            }

            QString tagText;
            if (arrayTag.size()) {
                tagText = db.getTagTreeText(arrayTag[0].strGUID);
            }

            ui->labelNotebook->setText("/" + db.name() + tagText + "/");
        }

        ui->labelTags->clear();
        ui->editAuthor->setText(data.strOwner);
    }

    // common fields
    ui->editCreateTime->setText(data.tCreated.toString());
    ui->editUpdateTime->setText(data.tModified.toString());
    ui->editURL->setText(data.strURL);
    ui->labelOpenURL->setText(WizFormatString2("<a href=\"%1\">%2</a>", data.strURL, tr("Open")));
    ui->labelSize->setText(sz);
    ui->checkEncrypted->setChecked(data.nProtected ? true : false);
}
