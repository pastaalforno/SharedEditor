#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFontDialog>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QTextBlock>
#include <QTimer>
#include <QtWidgets>
#include <typeinfo>
#if defined(QT_PRINTSUPPORT_LIB)
#include <QtPrintSupport/qtprintsupportglobal.h>
#if QT_CONFIG(printer)
#if QT_CONFIG(printdialog)
#include <QPrintDialog>
#endif
#include <QPrinter>
#if QT_CONFIG(printpreviewdialog)
#include <QPrintPreviewDialog>
#endif
#endif
#endif
#include "client.h"
#include "editor.h"
#include "ui_editor.h"

Editor::Editor(QWidget *parent, Client *client)
    : QMainWindow(parent), ui(new Ui::Editor), client(client) {
  ui->setupUi(this);
  QPixmap share(":/images/share");
  undoFlag = false;
  crdt = new CRDT(client);
  highlighter = new Highlighter(0, crdt);

  // Setup popup
  this->popUp = new QMessageBox(this);
  this->popUp->setText("Link copied to clipboard.");
  this->popUp->setWindowTitle("Shared Link");
  this->popUp->setStandardButtons(this->popUp->NoButton);
  this->popUp->setModal(false);
  this->popUp->setIconPixmap(share.scaled(
      30, 30, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
  this->popUp->setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);

  // Connect actions
  connect(ui->actionPrint, &QAction::triggered, this, &Editor::printPdf);
  connect(ui->actionExit, &QAction::triggered, this, &Editor::exit);
  connect(ui->actionCopy, &QAction::triggered, this, &Editor::copy);
  connect(ui->actionCut, &QAction::triggered, this, &Editor::cut);
  connect(ui->actionPaste, &QAction::triggered, this, &Editor::paste);
  connect(ui->actionUndo, &QAction::triggered, this, &Editor::undo);
  connect(ui->actionRedo, &QAction::triggered, this, &Editor::redo);
  connect(ui->actionFont, &QAction::triggered, this, &Editor::selectFont);
  connect(ui->actionBold, &QAction::triggered, this, &Editor::setFontBold);
  connect(ui->actionUnderline, &QAction::triggered, this,
          &Editor::setFontUnderline);
  connect(ui->actionItalic, &QAction::triggered, this, &Editor::setFontItalic);
  connect(ui->actionSharedLink, &QAction::triggered, this, &Editor::sharedLink);

  // Connect with client
  connect(client, &Client::usersConnectedReceived, this, &Editor::addUsers);
  connect(client, &Client::contentReceived, this, &Editor::updateText);
  connect(client, &Client::userDisconnected, this, &Editor::removeUser);
  connect(client, &Client::addCRDTterminator, this,
          &Editor::on_addCRDTterminator);
  connect(client, &Client::remoteCursor, this, &Editor::on_remoteCursor);
  connect(client, &Client::loggedIn, this, [this] {
    int site_id = fromStringToIntegerHash(this->client->getUsername());
    this->crdt->setId(site_id);
    this->highlighter->addLocal(site_id);
  });

  // Connect with textedit
  connect(ui->textEdit, &QTextEdit::textChanged, this, &Editor::textChange);
  connect(ui->textEdit->document(), &QTextDocument::contentsChange, this,
          &Editor::on_contentsChange);
  connect(ui->textEdit, &QTextEdit::cursorPositionChanged, this,
          &Editor::saveCursorPosition);
  connect(ui->textEdit, &QTextEdit::currentCharFormatChanged, this,
          &Editor::on_currentCharFormatChanged);

  // Connect with crdt
  connect(crdt, &CRDT::insert, this, &Editor::on_insert);
  connect(crdt, &CRDT::insertGroup, this, &Editor::on_insertGroup);
  connect(crdt, &CRDT::erase, this, &Editor::on_erase);
  connect(crdt, &CRDT::change, this, &Editor::on_change);
  connect(crdt, &CRDT::changeAlignment, this, &Editor::on_changeAlignment);

  // Add font, size and color to toolbar (cannot be otherwise achieved
  // using Qt creator GUI):
  // 1. Font
  comboFont = new QFontComboBox(ui->toolBar);
  ui->toolBar->addWidget(comboFont);
  connect(comboFont, QOverload<const QString &>::of(&QComboBox::activated),
          this, &Editor::textFamily);
  comboFont->setCurrentFont(QFont("American Typewriter"));

  // 2. Size
  comboSize = new QComboBox(ui->toolBar);
  ui->toolBar->addWidget(comboSize);

  QList<int> standardSizes = QFontDatabase::standardSizes();
  standardSizes.append(15);
  std::sort(standardSizes.begin(), standardSizes.end());
  foreach (int size, standardSizes)
    comboSize->addItem(QString::number(size));

  comboSize->setCurrentIndex(standardSizes.indexOf(15));
  connect(comboSize, QOverload<const QString &>::of(&QComboBox::activated),
          this, &Editor::textSize);

  // 3. Color
  QPixmap pix(16, 16);
  pix.fill(Qt::black);
  actionTextColor =
      ui->toolBar->addAction(pix, tr("&Color..."), this, &Editor::textColor);

  // Undo/redo config
  connect(ui->textEdit->document(), &QTextDocument::undoAvailable,
          ui->actionUndo, &QAction::setEnabled);
  connect(ui->textEdit->document(), &QTextDocument::redoAvailable,
          ui->actionRedo, &QAction::setEnabled);
  connect(ui->textEdit, &MyQTextEdit::undo, this, &Editor::undo);
  connect(ui->textEdit, &MyQTextEdit::redo, this, &Editor::redo);
  ui->actionUndo->setEnabled(ui->textEdit->document()->isUndoAvailable());
  ui->actionRedo->setEnabled(ui->textEdit->document()->isRedoAvailable());

  // Copy/paste/cut config
#ifndef QT_NO_CLIPBOARD
  ui->actionCut->setEnabled(false);
  connect(ui->textEdit, &QTextEdit::copyAvailable, ui->actionCut,
          &QAction::setEnabled);
  ui->actionCopy->setEnabled(false);
  connect(ui->textEdit, &QTextEdit::copyAvailable, ui->actionCopy,
          &QAction::setEnabled);
  connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
          &Editor::clipboardDataChanged);
#endif

  // Add alignment icons
  const QIcon leftIcon =
      QIcon::fromTheme("format-justify-left", QIcon(":/images/textleft.png"));
  actionAlignLeft = new QAction(leftIcon, tr("&Left"), this);
  actionAlignLeft->setShortcut(Qt::CTRL + Qt::Key_L);
  actionAlignLeft->setCheckable(true);
  actionAlignLeft->setPriority(QAction::LowPriority);
  const QIcon centerIcon = QIcon::fromTheme("format-justify-center",
                                            QIcon(":/images/textcenter.png"));
  actionAlignCenter = new QAction(centerIcon, tr("C&enter"), this);
  actionAlignCenter->setShortcut(Qt::CTRL + Qt::Key_E);
  actionAlignCenter->setCheckable(true);
  actionAlignCenter->setPriority(QAction::LowPriority);
  const QIcon rightIcon =
      QIcon::fromTheme("format-justify-right", QIcon(":/images/textright.png"));
  actionAlignRight = new QAction(rightIcon, tr("&Right"), this);
  actionAlignRight->setShortcut(Qt::CTRL + Qt::Key_R);
  actionAlignRight->setCheckable(true);
  actionAlignRight->setPriority(QAction::LowPriority);
  ui->toolBar->addAction(actionAlignLeft);
  ui->toolBar->addAction(actionAlignCenter);
  ui->toolBar->addAction(actionAlignRight);

  QActionGroup *alignGroup = new QActionGroup(this);
  connect(alignGroup, &QActionGroup::triggered, this, &Editor::textAlign);
  alignGroup->addAction(actionAlignLeft);
  alignGroup->addAction(actionAlignCenter);
  alignGroup->addAction(actionAlignRight);
  ui->toolBar->addSeparator();
  ui->toolBar->addActions(alignGroup->actions());
  ui->toolBar->addSeparator();
  connect(ui->textEdit, &MyQTextEdit::resetDefaultAlignment, actionAlignLeft,
          &QAction::trigger);

  // Show assigned text
  const QIcon assigned =
      QIcon::fromTheme("Cursor", QIcon(":/images/cursor.png"));
  actionShowAssigned = new QAction(assigned, tr("Cursor"), this);
  actionShowAssigned->setCheckable(true);
  actionShowAssigned->setChecked(false);
  ui->toolBar->addAction(actionShowAssigned);
  connect(actionShowAssigned, &QAction::triggered, this,
          &Editor::on_showAssigned);

  ui->textEdit->setLine(&line);
  ui->textEdit->setIndex(&index);
}

int Editor::fromStringToIntegerHash(QString str) {
  auto hash = QCryptographicHash::hash(str.toLatin1(), QCryptographicHash::Md5);

  QDataStream data(hash);
  int intHash;
  data >> intHash;
  return intHash;
}

void Editor::on_showAssigned() {
  if (this->highlighter->document() == 0) {
    this->highlighter->setDocument(ui->textEdit->document());
  } else {
    this->highlighter->setDocument(0);
  }
}

Editor::~Editor() { delete ui; }

void Editor::textChange() {}

void Editor::printPdf() {
#ifndef QT_NO_PRINTER
  //! [0]
  QFileDialog fileDialog(this, tr("Export PDF"));
  fileDialog.setAcceptMode(QFileDialog::AcceptSave);
  fileDialog.setMimeTypeFilters(QStringList("application/pdf"));
  fileDialog.setDefaultSuffix("pdf");
  if (fileDialog.exec() != QDialog::Accepted)
    return;
  QString fileName = fileDialog.selectedFiles().first();
  QPrinter printer(QPrinter::HighResolution);
  printer.setOutputFormat(QPrinter::PdfFormat);
  printer.setOutputFileName(fileName);
  ui->textEdit->document()->print(&printer);
  QString msg = tr("  Exported \"%1\"").arg(QDir::toNativeSeparators(fileName));

  statusBar()->showMessage(msg, 3000);
  //! [0]
#endif
}

void Editor::exit() {
  this->clear(false);
  crdt->setId(fromStringToIntegerHash(client->getUsername()));
  this->highlighter->addLocal(fromStringToIntegerHash(client->getUsername()));

  if (actionShowAssigned->isChecked()) {
    actionShowAssigned->trigger();
  } else if (this->highlighter->document() != 0) {
    this->highlighter->setDocument(0);
  }

  emit changeWidget(HOME);
}

void Editor::closeEvent(QCloseEvent *) { this->clear(false); }

// Add profile image in the peer bar (on the right of the editor)
QPixmap Editor::addImageInPeerBar(const QPixmap &orig, QColor color) {
  // Getting size if the original picture is not square
  int size = qMin(orig.width(), orig.height());
  // Creating circle clip area
  QPixmap rounded = QPixmap(size, size);
  rounded.fill(Qt::transparent);
  QPainterPath path;
  path.addEllipse(rounded.rect());
  QPainter painter(&rounded);
  painter.setClipPath(path);

  // Filling rounded area if needed
  painter.fillRect(rounded.rect(), Qt::black);
  // Getting offsets if the original picture is not square
  int x = qAbs(orig.width() - size) / 2;
  int y = qAbs(orig.height() - size) / 2;
  painter.drawPixmap(-x, -y, orig.width(), orig.height(), orig);

  QPixmap background = QPixmap(size + 50, size + 50);
  background.fill(Qt::transparent);
  QPainterPath path1;
  path1.addEllipse(background.rect());
  QPainter painter1(&background);
  painter1.setClipPath(path1);
  // Filling rounded area if needed
  painter1.fillRect(background.rect(), color);
  // Getting offsets if the original picture is not square
  x = qAbs(rounded.width() - size - 50) / 2;
  y = qAbs(rounded.height() - size - 50) / 2;
  painter1.drawPixmap(x, y, rounded.width(), rounded.height(), rounded);

  return background;
}

void Editor::peerYou() {
  QListWidgetItem *item = new QListWidgetItem();

  QPixmap orig = *client->getProfile();
  QPixmap background = addImageInPeerBar(orig, QColor(0, 136, 86));

  item->setIcon(QIcon(background));
  item->setText(this->client->getNickname() + " (You)");
  item->setData(Qt::UserRole, this->client->getUsername());
  item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
  item->setWhatsThis(this->client->getUsername());
  this->ui->listWidget->addItem(item);
}

void Editor::copy() {
#if QT_CONFIG(clipboard)
  ui->textEdit->copy();
#endif
}

void Editor::cut() {
#if QT_CONFIG(clipboard)
  ui->textEdit->cut();
#endif
}

void Editor::paste() {
#if QT_CONFIG(clipboard)
  ui->textEdit->paste();
#endif
}

void Editor::undo() {
  this->undoFlag = true;
  ui->textEdit->document()->undo(); // the change due to the insert/delete
                                    // are automatically managed
                                    // by on_contents_change
  // Update alignment icon
  alignmentChanged(ui->textEdit->alignment());
}

void Editor::redo() {
  this->redoFlag = true;
  ui->textEdit->document()->redo();

  // Update alignment icon
  alignmentChanged(ui->textEdit->alignment());
}

void Editor::selectFont() {
  bool fontSelected;
  QFont font = QFontDialog::getFont(&fontSelected, this);
  if (fontSelected)
    ui->textEdit->setFont(font);
}

void Editor::setFontUnderline(bool underline) {
  ui->textEdit->setFontUnderline(underline);
  on_formatChange();
}

void Editor::setFontItalic(bool italic) {
  ui->textEdit->setFontItalic(italic);
  on_formatChange();
}

void Editor::sharedLink() {
  QClipboard *clipboard = QGuiApplication::clipboard();
  clipboard->setText(client->getSharedLink());

  QTimer::singleShot(1000, this->popUp, &QMessageBox::hide); // 1000 ms
  this->popUp->show();
}

void Editor::setFontBold(bool bold) {
  bold ? ui->textEdit->setFontWeight(QFont::Bold)
       : ui->textEdit->setFontWeight(QFont::Normal);
  on_formatChange();
}

void Editor::textAlign(QAction *a) {
  QString changed = ui->textEdit->textCursor().selectedText();
  QTextCursor cursor = ui->textEdit->textCursor();
  int start = ui->textEdit->textCursor().selectionStart();
  cursor.setPosition(start);
  int line_start = cursor.blockNumber();
  int end = ui->textEdit->textCursor().selectionEnd();
  cursor.setPosition(end);
  int line_end = cursor.blockNumber();

  QTextBlockFormat n;
  SymbolFormat::Alignment sf;
  if (a == actionAlignLeft) {
    sf = SymbolFormat::Alignment::ALIGN_LEFT;
    n.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
  } else if (a == actionAlignRight) {
    sf = SymbolFormat::Alignment::ALIGN_RIGHT;
    n.setAlignment(Qt::AlignRight | Qt::AlignAbsolute);
  } else if (a == actionAlignCenter) {
    sf = SymbolFormat::Alignment::ALIGN_CENTER;
    n.setAlignment(Qt::AlignHCenter);
  }

  for (int i = line_start; i <= line_end; i++)
    crdt->localChangeAlignment(i, sf);

  ui->textEdit->textCursor().mergeBlockFormat(n);
}

SymbolFormat::Alignment Editor::alignmentConversion(Qt::Alignment a) {
  if (a == (Qt::AlignLeft | Qt::AlignLeading) ||
      a == (Qt::AlignLeading | Qt::AlignAbsolute)) {
    return SymbolFormat::Alignment::ALIGN_LEFT;
  } else if (a == Qt::AlignCenter || a == Qt::AlignHCenter) {
    return SymbolFormat::Alignment::ALIGN_CENTER;
  } else if (a == Qt::AlignRight ||
             a == (Qt::AlignTrailing | Qt::AlignAbsolute)) {
    return SymbolFormat::Alignment::ALIGN_RIGHT;
  }
}

Qt::Alignment Editor::alignmentConversion(SymbolFormat::Alignment a) {
  if (a == SymbolFormat::Alignment::ALIGN_LEFT) {
    return (Qt::AlignLeft | Qt::AlignLeading);
  } else if (a == SymbolFormat::Alignment::ALIGN_CENTER) {
    return Qt::AlignHCenter;
  } else if (a == SymbolFormat::Alignment::ALIGN_RIGHT) {
    return Qt::AlignTrailing | Qt::AlignAbsolute;
  }
}

/****************************************************
        LOCAL OPERATION: update textedit THEN crdt
        REMOTE OPERATION: update crdt THEN textedit
****************************************************/
void Editor::on_contentsChange(int position, int charsRemoved, int charsAdded) {
  // REMOTE OPERATION: insert/delete received from remote client:
  // nothing to update
  if (((charsAdded - charsRemoved) > 0 &&
       ui->textEdit->toPlainText().size() <= crdt->getSize()) ||
      ((charsRemoved - charsAdded) > 0 &&
       ui->textEdit->toPlainText().size() >= crdt->getSize())) {
    return;
  }

  // LOCAL OPERATION: insert/deleted performed in this editor:
  // update CRDT structure
  // "charsAdded - charsRemoved" and "charsRemoved - charsAdded" are conditions
  // added to handle QTextDocument::contentsChange bug QTBUG-3495

  qDebug()<<"Inserted "<<ui->textEdit->getInserted();

  //substitute selection
  if (ui->textEdit->getSelected() && charsAdded>0 && charsRemoved>0 && ui->textEdit->getInserted()){
      qDebug()<<"qui si deve operare";
          disconnect(ui->textEdit->document(), &QTextDocument::contentsChange, this,
                     &Editor::on_contentsChange);
          disconnect(ui->textEdit, &QTextEdit::cursorPositionChanged, this,
                     &Editor::saveCursorPosition);
          QString removed;


          ui->textEdit->document()->undo();
          removed =
                ui->textEdit->document()->toPlainText().mid(position, charsRemoved);
          ui->textEdit->document()->redo();

          connect(ui->textEdit->document(), &QTextDocument::contentsChange, this,
                  &Editor::on_contentsChange);
          connect(ui->textEdit, &QTextEdit::cursorPositionChanged, this,
                  &Editor::saveCursorPosition);
          QTextCursor tmp_cursor = ui->textEdit->textCursor();
          tmp_cursor.setPosition(position);
          int tmp_line = tmp_cursor.blockNumber();
          int tmp_index = tmp_cursor.positionInBlock();
          // remove multiple chars
          // qDebug()<<"Before remove line and index: "<<"("<<line<<","<<index<<")";
          if (removed.length()) {
            crdt->localErase(tmp_line, tmp_index, removed.length());
          }

          this->undoFlag = false;
          QString added =
              ui->textEdit->toPlainText().mid(position, charsAdded );
          qDebug()<<"added "<<added;
          if (added.at(0) == '\0')
            return;
          // move cursor before first char to insert
          QTextCursor cursor = ui->textEdit->textCursor();
          cursor.setPosition(position);
          // single character
          int line = cursor.blockNumber();
          int index = cursor.positionInBlock();
          if (charsAdded == 1) {

            // qDebug() << "Added " << added.at(0) << "in position (" << line << ","
            // << index << ")";

            // to retrieve the format it is necessary to be on the RIGHT of the target
            // char
            cursor.movePosition(QTextCursor::Right);
            QFont font = cursor.charFormat().font();
            ui->textEdit->update();
            crdt->localInsert(line, index, added.at(0).unicode(), font,
                              cursor.charFormat().foreground().color(),
                              getCurrentAlignment());
          } else {
            this->undoFlag = false;
            QFont fontPrec;
            QColor colorPrec;
            QString partial;
            QFont font;
            QColor color;
            Qt::Alignment align;
            Qt::Alignment alignPrec = ui->textEdit->document()
                                          ->findBlockByNumber(line)
                                          .blockFormat()
                                          .alignment();
            int linePrec = line;
            int numLines = line;
            // add multiple chars
            // qDebug() << "Multiple chars: position" << cursor.position()<<"
            // (line,index): ( "<<line<<","<< index<<")";
            for (int i = 0; i < charsAdded; i++) {

              // int line = cursor.blockNumber();
              // int index = cursor.positionInBlock();

              // to retrieve the format it is necessary to be on the RIGHT of the
              // target char
              cursor.movePosition(QTextCursor::Right);
              // qDebug()<<"Added at: "<<i<<" -> "<<added.at(i).unicode();
              font = cursor.charFormat().font();
              color = cursor.charFormat().foreground().color();
              align = alignPrec;
              if (i == 0) {
                fontPrec = font;
                colorPrec = color;
              }

              if (numLines != linePrec) {
                QTextBlock block =
                    ui->textEdit->document()->findBlockByNumber(numLines);
                QTextBlockFormat textBlockFormat = block.blockFormat();
                align = textBlockFormat.alignment();
                // qDebug()<<"line: "<<numLines<<" alignment: "<<align;
              }

              if (font == fontPrec && color == colorPrec && align == alignPrec) {
                // qDebug()<<"concatenated: "<<added.at(i).unicode();
                partial.append(added.at(i).unicode());
              } else {
                crdt->localInsertGroup(line, index, partial, fontPrec, colorPrec,
                                       alignPrec);
                // qDebug()<<"Inserted: "<<partial;
                fontPrec = font;
                colorPrec = color;
                alignPrec = align;
                partial.clear();
                partial.append(added.at(i).unicode());
              }

              linePrec = numLines;
              if (added.at(i) == '\n') {
                numLines++;
              }
            }

            if (!partial.isNull() && !partial.isEmpty()) {
              crdt->localInsertGroup(line, index, partial, font, color, align);
            }
          }
  }
  else if ( charsAdded > 0 && charsAdded - charsRemoved > 0) {

    this->undoFlag = false;
    QString added =
        ui->textEdit->toPlainText().mid(position, charsAdded - charsRemoved);
    if (added.at(0) == '\0')
      return;
    // move cursor before first char to insert
    QTextCursor cursor = ui->textEdit->textCursor();
    cursor.setPosition(position);
    // single character
    int line = cursor.blockNumber();
    int index = cursor.positionInBlock();
    if ((charsAdded - charsRemoved) == 1) {

      // qDebug() << "Added " << added.at(0) << "in position (" << line << ","
      // << index << ")";

      // to retrieve the format it is necessary to be on the RIGHT of the target
      // char
      cursor.movePosition(QTextCursor::Right);
      QFont font = cursor.charFormat().font();
      ui->textEdit->update();
      crdt->localInsert(line, index, added.at(0).unicode(), font,
                        cursor.charFormat().foreground().color(),
                        getCurrentAlignment());
    } else {
      this->undoFlag = false;
      QFont fontPrec;
      QColor colorPrec;
      QString partial;
      QFont font;
      QColor color;
      Qt::Alignment align;
      Qt::Alignment alignPrec = ui->textEdit->document()
                                    ->findBlockByNumber(line)
                                    .blockFormat()
                                    .alignment();
      int linePrec = line;
      int numLines = line;
      // add multiple chars
      // qDebug() << "Multiple chars: position" << cursor.position()<<"
      // (line,index): ( "<<line<<","<< index<<")";
      for (int i = 0; i < charsAdded - charsRemoved; i++) {

        // int line = cursor.blockNumber();
        // int index = cursor.positionInBlock();

        // to retrieve the format it is necessary to be on the RIGHT of the
        // target char
        cursor.movePosition(QTextCursor::Right);
        // qDebug()<<"Added at: "<<i<<" -> "<<added.at(i).unicode();
        font = cursor.charFormat().font();
        color = cursor.charFormat().foreground().color();
        align = alignPrec;
        if (i == 0) {
          fontPrec = font;
          colorPrec = color;
        }

        if (numLines != linePrec) {
          QTextBlock block =
              ui->textEdit->document()->findBlockByNumber(numLines);
          QTextBlockFormat textBlockFormat = block.blockFormat();
          align = textBlockFormat.alignment();
          // qDebug()<<"line: "<<numLines<<" alignment: "<<align;
        }

        if (font == fontPrec && color == colorPrec && align == alignPrec) {
          // qDebug()<<"concatenated: "<<added.at(i).unicode();
          partial.append(added.at(i).unicode());
        } else {
          crdt->localInsertGroup(line, index, partial, fontPrec, colorPrec,
                                 alignPrec);
          // qDebug()<<"Inserted: "<<partial;
          fontPrec = font;
          colorPrec = color;
          alignPrec = align;
          partial.clear();
          partial.append(added.at(i).unicode());
        }

        linePrec = numLines;
        if (added.at(i) == '\n') {
          numLines++;
        }
      }

      if (!partial.isNull() && !partial.isEmpty()) {
        crdt->localInsertGroup(line, index, partial, font, color, align);
      }
    }
    // Handle deletion
  } else if (charsRemoved > 0 && charsRemoved - charsAdded > 0) {
    // qDebug()<<"----DEL";
    // undo to retrieve the content deleted
    disconnect(ui->textEdit->document(), &QTextDocument::contentsChange, this,
               &Editor::on_contentsChange);
    disconnect(ui->textEdit, &QTextEdit::cursorPositionChanged, this,
               &Editor::saveCursorPosition);
    QString removed;
    if (this->undoFlag == true) {
      // qDebug()<<"redo - undo";
      ui->textEdit->document()->redo();
      removed =
          ui->textEdit->document()->toPlainText().mid(position, charsRemoved);
      ui->textEdit->document()->undo();
      this->undoFlag = false;
    } else {

      ui->textEdit->document()->undo();
      removed =
          ui->textEdit->document()->toPlainText().mid(position, charsRemoved);
      ui->textEdit->document()->redo();
    }
    connect(ui->textEdit->document(), &QTextDocument::contentsChange, this,
            &Editor::on_contentsChange);
    connect(ui->textEdit, &QTextEdit::cursorPositionChanged, this,
            &Editor::saveCursorPosition);
    saveCursorPosition();
    // remove multiple chars
    // qDebug()<<"Before remove line and index: "<<"("<<line<<","<<index<<")";
    if (removed.length()) {
      crdt->localErase(line, index, removed.length());
    }
    // qDebug()<<"After remove line and index: "<<"("<<line<<","<<index<<")";
  } else if (charsRemoved == charsAdded &&
             (this->undoFlag == true || this->redoFlag == true)) {
    // format/alignment change by redo/undo
    // qDebug()<<"format change";
    // save cursor position
    QTextCursor cursor = ui->textEdit->textCursor();
    cursor.setPosition(position);
    int line_m;
    int index_m;
    bool formatChange = false;
    // per ogni carattere nel documento lo confronto con quello nella struttura
    // dati
    while (true) {

      line_m = cursor.blockNumber();
      index_m = cursor.positionInBlock();

      // non sono sicuro di cosa succeda all'ultimo carattere
      if (cursor.movePosition(QTextCursor::NextCharacter,
                              QTextCursor::KeepAnchor) == false) {
        // qDebug()<<"ciclo break perchè movePosition failed";
        break;
      }

      QTextCharFormat formatDoc = cursor.charFormat();

      QTextCharFormat formatSL = this->crdt->getSymbolFormat(line_m, index_m);

      if (formatSL.font() == formatDoc.font() &&
          formatSL.foreground().color() == formatDoc.foreground().color()) {
        ////qDebug()<<"formatSL == formatDOC";

        break;
      }
      if (formatChange == false)
        formatChange = true;
    }

    if (formatChange == true) {
      // qDebug()<<"After the while true loop: "<<cursor.selectedText();
      on_formatChange(cursor);
    } else {
      // change alignment
      // TO DO: multiple lines alignment chnaged
      line_m = cursor.blockNumber();
      index_m = cursor.positionInBlock();
      QTextBlockFormat a = cursor.blockFormat();
      Qt::Alignment align = a.alignment();
      // qDebug()<<"First alignment from the editor:
      // "<<alignmentConversion(align);
      Qt::Alignment align_SL =
          alignmentConversion(this->crdt->getAlignmentLine(line_m));
      // qDebug()<<"First alignment  present in the crdt: "<<align_SL;
      this->crdt->localChangeAlignment(line_m, alignmentConversion(align));

      while (true) {
        // non sono sicuro di cosa succeda all'ultimo carattere
        if (cursor.movePosition(QTextCursor::NextBlock,
                                QTextCursor::MoveAnchor) == false) {
          ////qDebug()<<"ciclo break perchè movePosition failed";
          break;
        }
        a = cursor.blockFormat();
        align = a.alignment();
        // qDebug()<<"Alignment fromn the editor: "<<align;
        line_m = cursor.blockNumber();
        index_m = cursor.positionInBlock();
        align_SL = alignmentConversion(this->crdt->getAlignmentLine(line_m));
        // qDebug()<<"Alignment from the CRDT: "<<align_SL;
        this->crdt->localChangeAlignment(line_m, alignmentConversion(align));
      }
    }

    this->undoFlag = false;
    this->redoFlag = false;
  }
  ui->textEdit->setInserted(false);
}

void Editor::on_changeAlignment(int align, int line, int index) {
  QTextCursor cursor = ui->textEdit->textCursor();
  QTextBlock block = ui->textEdit->document()->findBlockByNumber(line);
  cursor.setPosition(block.position() + index);
  QTextBlockFormat textBlockFormat = block.blockFormat();
  if (align == SymbolFormat::Alignment::ALIGN_LEFT) {
    textBlockFormat.setAlignment(Qt::AlignLeft);
    if (line == this->line)
      actionAlignLeft->setChecked(true);
  } else if (align == SymbolFormat::Alignment::ALIGN_CENTER) {
    textBlockFormat.setAlignment(Qt::AlignCenter);
    if (line == this->line)
      actionAlignCenter->setChecked(true);
  } else if (align == SymbolFormat::Alignment::ALIGN_RIGHT) {
    textBlockFormat.setAlignment(Qt::AlignRight);
    if (line == this->line)
      actionAlignRight->setChecked(true);
  }
  cursor.mergeBlockFormat(textBlockFormat);
}

// Handle remote insert
void Editor::on_insert(int line, int index, const Symbol &s) {
  // qDebug() << "ON_INSERT REMOTE";
  QTextCursor cursor = ui->textEdit->textCursor();
  //    cursor.setPosition(index);

  QTextBlock block = ui->textEdit->document()->findBlockByNumber(line);
  cursor.setPosition(block.position() + index);

  // save old format to restore it later
  QTextCharFormat oldFormat = ui->textEdit->currentCharFormat();
  //    //qDebug() << "oldFormat" << oldFormat.font().italic() <<
  //    oldFormat.font().bold() << oldFormat.font().underline();
  QTextCharFormat newFormat = s.getQTextCharFormat();
  //    //qDebug() << "format" << newFormat.font().bold();
  cursor.setCharFormat(newFormat);
  cursor.insertText(QChar(s.getValue()));
  ui->textEdit->setCurrentCharFormat(oldFormat);
  ui->textEdit->update();

  // qDebug().noquote() << crdt->to_string();
  //  QTextCharFormat f = cursor.charFormat();

  //  qDebug() << "CursorFORMAT" << f.font().pointSize();
  qDebug() << line << index;
  qDebug() << this->line << this->index;
  qDebug() << cursor.blockNumber() << cursor.positionInBlock();
  //  on_formatChange();
  //  on_formatChange(cursor);
}

// Handle remote group insertion
void Editor::on_insertGroup(int line, int index, const QString &s,
                            QTextCharFormat newFormat) {
  // qDebug() << "ON_INSERT";
  QTextCursor cursor = ui->textEdit->textCursor();
  //    cursor.setPosition(index);

  QTextBlock block = ui->textEdit->document()->findBlockByNumber(line);
  cursor.setPosition(block.position() + index);

  // save old format to restore it later
  QTextCharFormat oldFormat = ui->textEdit->currentCharFormat();
  //    //qDebug() << "oldFormat" << oldFormat.font().italic() <<
  //    oldFormat.font().bold() << oldFormat.font().underline();

  //    //qDebug() << "format" << newFormat.font().bold();
  cursor.setCharFormat(newFormat);
  cursor.insertText(s);
  ui->textEdit->setCurrentCharFormat(oldFormat);

  // qDebug().noquote() << crdt->to_string();
}

void Editor::on_erase(int line, int index, int lenght) {
  // qDebug()<<"ON ERASE";
  QTextCursor cursor = ui->textEdit->textCursor();
  //    //qDebug() << line << index;
  QTextBlock block = ui->textEdit->document()->findBlockByNumber(line);
  cursor.setPosition(block.position() + index);
  for (int i = 0; i < lenght; i++) {
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
  }
  //    //qDebug() << "block position" << block.position();

  //    //qDebug() << "before deleting";
  cursor.removeSelectedText();
  //    //qDebug() << "after deleting";

  // qDebug().noquote() << crdt->to_string();
}

void Editor::on_change(const QVector<Symbol> &symbols) {
  qDebug() << "ON_CHANGE";

  QTextCursor tempCursor=ui->textEdit->textCursor();
  bool first=true;

  QTextCharFormat newFormat;

  for (Symbol s : symbols) {
    int line, index;
    this->crdt->findPosition(s, line, index);
    qDebug() << "changing in (line,index) " << line << " " << index;
    // QTextCursor cursor = ui->textEdit->textCursor();
    QTextBlock block = ui->textEdit->document()->findBlockByNumber(line);
    // cursor.setPosition(block.position() + index);

    // save old format to restore it later
    // QTextCharFormat oldFormat = ui->textEdit->currentCharFormat();

    //    cursor.setCharFormat(newFormat);
    //    cursor.insertText(QString(1, s.getValue()));
    // tempCursor = cursor;
    if (first) {
      first = false;
      tempCursor.setPosition(block.position() + index);
      newFormat = s.getQTextCharFormat();
    }

    tempCursor.setPosition(block.position() + index + 1,
                           QTextCursor::KeepAnchor);

    // ui->textEdit->setCurrentCharFormat(oldFormat);
  }
  //(!first)
  tempCursor.setCharFormat(newFormat);
  qDebug() << "Selected" << tempCursor.selection().toPlainText();

  // qDebug().noquote() << crdt->to_string();
}

// TODO: da cambiare
void Editor::updateText(const QString &text) {
  ui->listWidget->clear();
  QListWidgetItem *item = new QListWidgetItem;

  QPixmap orig = *client->getProfile();
  QPixmap background = addImageInPeerBar(orig, QColor(0, 136, 86));

  item->setIcon(QIcon(background));
  item->setText(client->getUsername());
  this->ui->listWidget->addItem(item);
  this->ui->textEdit->setText(text);
}

void Editor::addUsers(
    const QList<QPair<QPair<QString, QString>, QPixmap>> users) {
  for (int i = 0; i < users.count(); i++) {
    int user = fromStringToIntegerHash(users.at(i).first.first);
    if (highlighter->addClient(user)) { // Prevents duplicates
      if (this->highlighter->document() != 0) {
        this->highlighter->setDocument(ui->textEdit->document());
      }
      if (ui->textEdit->remote_cursors.contains(user)) {
        RemoteCursor *remote_cursor = ui->textEdit->remote_cursors.value(user);
        remote_cursor->setColor(this->highlighter->getColor(user));
      }

      QListWidgetItem *item = new QListWidgetItem();

      QPixmap orig = users.at(i).second;
      QPixmap background = addImageInPeerBar(orig, highlighter->getColor(user));

      item->setIcon(QIcon(background));
      item->setText(users.at(i).first.second);
      item->setData(Qt::UserRole, users.at(i).first.first);
      item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
      item->setWhatsThis(users.at(i).first.first);
      this->ui->listWidget->addItem(item);
    }

  } // TODO: per ora è visualizzato l'username per faciliatare la cancellazione
    // senza riferimenti alla riga
}

void Editor::clear(bool serverDisconnected) {
  if (!serverDisconnected)
    client->closeFile();
  highlighter->freeAll();
  // clean the editor: disconnect...
  disconnect(ui->textEdit->document(), &QTextDocument::contentsChange, this,
             &Editor::on_contentsChange);
  disconnect(client, &Client::remoteCursor, this, &Editor::on_remoteCursor);
  disconnect(ui->textEdit, &QTextEdit::cursorPositionChanged, this,
             &Editor::saveCursorPosition);
  // create new CRDT with connections
  crdt->clear();
  ui->listWidget->clear();
  ui->textEdit->clear();

  actionAlignLeft->setChecked(false);
  actionAlignCenter->setChecked(false);
  actionAlignRight->setChecked(false);

  // ... and then riconnect (because we want to remove chars locally without
  // deleteling them in server)
  connect(ui->textEdit->document(), &QTextDocument::contentsChange, this,
          &Editor::on_contentsChange);
  connect(client, &Client::remoteCursor, this, &Editor::on_remoteCursor);
  connect(ui->textEdit, &QTextEdit::cursorPositionChanged, this,
          &Editor::saveCursorPosition);
}

void Editor::removeUser(const QString &username, const QString &nickname) {
  QList<QListWidgetItem *> items =
      this->ui->listWidget->findItems(nickname, Qt::MatchFixedString);

  for (QListWidgetItem *item : items) {
    if (item->data(Qt::UserRole).toString() == username) {
      // qDebug()<<"ITEM TO REMOVE: "<<username;
      highlighter->freeColor(fromStringToIntegerHash(username));
      if (this->highlighter->document() != 0) {
        // qDebug()<< "Assigning file";
        this->highlighter->setDocument(ui->textEdit->document());
        // qDebug()<< "Assigned file";
      }
      this->ui->listWidget->removeItemWidget(item);
      ui->textEdit->remote_cursors.remove(fromStringToIntegerHash(username));
      delete item;
      break;
    }
  }
}

void Editor::saveCursorPosition() {
  // Update alignment icon
  alignmentChanged(ui->textEdit->alignment());

  // Save cursor position
  QTextCursor cursor = ui->textEdit->textCursor();
  this->line = cursor.blockNumber();
  this->index = cursor.positionInBlock();
  fontChanged(cursor.charFormat().font());

  // use positon of symbol AFTER cursor as reference
  // qDebug() << "cursor position before" << this->line << this->index;
  // correct_position(this->line, this->index);
  crdt->cursorPositionChanged(this->line, this->index);
}

void Editor::showEvent(QShowEvent *) {
  QString windowTitle = client->getOpenedFile().replace(",", " (") + ")";
  this->setWindowTitle(windowTitle + " - Shared Editor");
  moveCursorToEnd();
}

// Update icons in toolbar (italic, bold, ...)
// depending on the char before cursor
void Editor::on_currentCharFormatChanged(const QTextCharFormat &format) {
  fontChanged(format.font());
  colorChanged(format.foreground().color());
  void on_remoteCursor(int editor_id, Symbol s);
}

void Editor::textColor() {
  QColor col = QColorDialog::getColor(ui->textEdit->textColor(), this);
  if (!col.isValid())
    return;
  ui->textEdit->setTextColor(col);
  on_formatChange();
}

void Editor::clipboardDataChanged() {
#ifndef QT_NO_CLIPBOARD
  if (const QMimeData *md = QApplication::clipboard()->mimeData())
    ui->actionPaste->setEnabled(md->hasText());
#endif
}

void Editor::fontChanged(const QFont &f) {
  comboFont->setCurrentIndex(comboFont->findText(QFontInfo(f).family()));
  comboSize->setCurrentIndex(
      comboSize->findText(QString::number(f.pointSize())));
  ui->actionBold->setChecked(f.bold());
  ui->actionItalic->setChecked(f.italic());
  ui->actionUnderline->setChecked(f.underline());
}

void Editor::colorChanged(const QColor &c) {
  QPixmap pix(16, 16);
  pix.fill(c);
  actionTextColor->setIcon(pix);
}

void Editor::alignmentChanged(Qt::Alignment a) {
  if (a & Qt::AlignLeft)
    actionAlignLeft->setChecked(true);
  else if (a & Qt::AlignHCenter)
    actionAlignCenter->setChecked(true);
  else if (a & Qt::AlignRight)
    actionAlignRight->setChecked(true);
}

Qt::Alignment Editor::getCurrentAlignment() {
  if (actionAlignLeft->isChecked())
    return Qt::AlignLeft;
  else if (actionAlignCenter->isChecked())
    return Qt::AlignCenter;
  else if (actionAlignRight->isChecked())
    return Qt::AlignRight;
}

void Editor::textFamily(const QString &f) {
  ui->textEdit->setFontFamily(f);
  on_formatChange();
  ui->textEdit->setFocus();
}

void Editor::textSize(const QString &p) {
  qreal pointSize = p.toFloat();
  if (p.toFloat() > 0) {
    ui->textEdit->setFontPointSize(pointSize);
  }
  on_formatChange();
  ui->textEdit->setFocus();
}

void Editor::moveCursorToEnd() {
  // Move the cursor to end of the text
  QTextCursor cursor(ui->textEdit->document());
  cursor.movePosition(QTextCursor::End);
  ui->textEdit->setTextCursor(cursor);
}

// Handle local format change
void Editor::on_formatChange(const QString &changed, int start, int end) {
  QFont fontPrec;
  QColor colorPrec;
  QFont font;
  QColor color;
  int startIndex;
  int endIndex;
  int startLine;
  int endLine;

  QTextCursor cursor = ui->textEdit->textCursor();
  if (start == end) {
       return;
    cursor.setPosition(start);
    int line = cursor.blockNumber();
    int index = cursor.positionInBlock();

    startLine = endLine = line;
    startIndex = endIndex = index;
  }
  qDebug() << "start/end selection" << start << end;

  for (int i = start; i < end; i++) {
    cursor.setPosition(i);
    int line = cursor.blockNumber();
    int index = cursor.positionInBlock();
    if(i==start){
        startIndex=endIndex=index;
        startLine=endLine=line;
    }

    //qDebug() << "line/index/char" << line << index << changed.at(i - start);

    // If newline ('\n') do nothing
    if (changed.at(i - start) == QChar(0x2029)) {
      continue;
    }

    // qDebug() << "Font: " << font;
    // qDebug() << "FontPrec: " << fontPrec;

    // qDebug() << "Color: " << color;
    // qDebug() << "ColorPrec: " << colorPrec;

    // Position AFTER the char to read its format
    cursor.setPosition(i + 1);

    font = cursor.charFormat().font();
    color = cursor.charFormat().foreground().color();
    if (i == start) {
      fontPrec = font;
      colorPrec = color;
    }

    if (font == fontPrec && color == colorPrec) {
      // qDebug()<<"concatenated: "<<added.at(i).unicode();
      endIndex = index;
      endLine = line;
    } else {
        qDebug() << "Local change group; startLine: " << startLine
                 << " endLine: " << endLine << " startIndex: " << startIndex
                 << " endIndex: " << endIndex;
      crdt->localChangeGroup(startLine, endLine, startIndex, endIndex, fontPrec,
                             colorPrec);

      fontPrec = font;
      colorPrec = color;
      startIndex = index;
      startLine = line;
      endIndex = index;
      endLine = line;
    }
  }
  qDebug() << "Local change group; startLine: " << startLine
           << " endLine: " << endLine << " startIndex: " << startIndex
           << " endIndex: " << endIndex;

  crdt->localChangeGroup(startLine, endLine, startIndex, endIndex, fontPrec,
                         colorPrec);

}

void Editor::on_formatChange() {
  QString changed = ui->textEdit->textCursor().selectedText();
  int start = ui->textEdit->textCursor().selectionStart();
  int end = ui->textEdit->textCursor().selectionEnd();

  on_formatChange(changed, start, end);
}

void Editor::on_formatChange(QTextCursor c) {
  QString changed = c.selectedText();
  int start = c.selectionStart();
  int end = c.selectionEnd();

  on_formatChange(changed, start, end);
}

void Editor::on_addCRDTterminator() {
  QFont font;
  QColor color;
  this->crdt->localInsert(0, 0, '\0', font, color, getCurrentAlignment());
}

void Editor::on_remoteCursor(int editor_id, Symbol s) {
  int line, index;
  crdt->getPositionFromSymbol(s, line, index);

  QTextBlock block = ui->textEdit->document()->findBlockByNumber(line);
  if (!ui->textEdit->remote_cursors.contains(editor_id)) {
    // Add new cursor
    RemoteCursor *remote_cursor =
        new RemoteCursor(ui->textEdit->textCursor(), block, index,
                         highlighter->getColor(editor_id));
    ui->textEdit->remote_cursors.insert(editor_id, remote_cursor);
  } else {
    // Update already existing cursor
    RemoteCursor *remote_cursor = ui->textEdit->remote_cursors.value(editor_id);

    remote_cursor->moveTo(block, index);
  }
}
