#ifndef _MAINWINDOW_H_
#define _MAINWINDOW_H_

#include "keyboard.h"
#include "ui_mainwindow.h"
#include "wpap2p.h"

class MainWindow : public QWidget, public Ui::MainWindow
{
Q_OBJECT

public:
    MainWindow(QWidget *parent = 0);
    virtual ~MainWindow();

private slots:
    void focusChanged(QWidget *old, QWidget *now);
    void groupStarted();
    void groupStopped();
    void setWifiDirectEnabled(bool state);

public slots:
    void backClicked();
    void enableStateChanged(int state);
    void exitClicked();
    void settingsClicked();

private:
    Keyboard *keyboard;
    WPAp2p wpa;
};


#endif /* _MAINWINDOW_H_ */
