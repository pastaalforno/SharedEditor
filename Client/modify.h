#ifndef MODIFY_PROFILE_H
#define MODIFY_PROFILE_H

#include "appMainWindow.h"
#include "client.h"
#include <QMessageBox>
#include <QWidget>

#define IMAGE_WIDTH 170
#define IMAGE_HEIGHT 170

namespace Ui {
class modify;
}

class Modify : public QWidget {
  Q_OBJECT

public:
  Modify(QWidget *parent = nullptr, Client *client = nullptr);
  ~Modify();

signals:
  void changeWidget(int);

private slots:
  void on_profile_image_clicked();
  void on_lineEditNickname_editingFinished();
  void on_lineEditNickname_textChanged(const QString &arg1);
  void on_lineEditNewPass_textChanged(const QString &arg1);

  void on_lineEditNewPass_editingFinished();
  void on_lineEditConfirmPass_editingFinished();
  void on_lineEditConfirmPass_textChanged();
  void on_pushSaveNickname_clicked();
  void on_pushButtonSavePassword_clicked();
  void save_photo();
  void on_t_pushButtonFinish_clicked();
  void on_lineEditOldPass_editingFinished();
  void on_wrongOldPasswordEntered(const QString &reason);
  void on_correctOldPasswordEntered();
  void on_failedUpdatePassword(const QString &reason);
  void on_successUpdatePassword();
  void on_lineEditOldPass_textChanged(const QString &arg1);

public slots:
  void upload();

private:
  Ui::modify *ui;
  Client *client;
  QPixmap *profile_photo_temp;
  bool valid_new_password = false;
  enum OldPassStatus {
    CORRECT,
    WRONG,
    UNCHECKED
  } correct_old_password = UNCHECKED;

  QMessageBox *popUp;
  QPixmap *success;
  QPixmap *failed;

  void clearLabels();
  void checkPassword(const QString &password);
  bool checkConfirmation(const QString &pass, const QString &conf);
  bool checkNickname(const QString &nickname);
  void addPasswordError(QString error);
  void clearOldPasswordError();
  void clearNewPasswordError();
  void clearConfirmPasswordError();
  void clearForm();
  void addErrorNickname(QString error);
  void clearErrorNickname();
};

#endif // MODIFY_PROFILE_H
