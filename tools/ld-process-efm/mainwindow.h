/************************************************************************

    mainwindow.h

    ld-process-efm - EFM data decoder
    Copyright (C) 2019 Simon Inns

    This file is part of ld-decode-tools.

    ld-process-efm is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QDebug>

#include "configuration.h"
#include "efmprocess.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void processingCompleteSignalHandler(void);

    void on_actionOpen_EFM_File_triggered();
    void on_actionExit_triggered();
    void on_decodePushButton_clicked();
    void on_cancelPushButton_clicked();

private:
    Ui::MainWindow *ui;

    // Class globals
    Configuration *configuration;
    EfmProcess efmProcess;
    QLabel efmStatus;
    QString currentInputEfmFileAndPath;

    // Method prototypes
    void guiNoEfmFileLoaded(void);
    void guiEfmFileLoaded(void);
    void guiEfmProcessingStop(void);
    void guiEfmProcessingStart(void);
    void loadInputEfmFile(QString filename);
};

#endif // MAINWINDOW_H
