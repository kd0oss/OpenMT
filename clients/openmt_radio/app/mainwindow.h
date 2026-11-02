#ifndef OPENMT_MAINWINDOW_H
#define OPENMT_MAINWINDOW_H

#include <QMainWindow>
#include <QVector>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QFrame;
class QEvent;
class QResizeEvent;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QSlider;
class QKeyEvent;

class AudioIO;
class RadioController;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onConnect();
    void onRelease();
    void onDtmfToggle(bool on);
    void onDtmfKey();
    void placeDtmfPad();
    void openDongle();
    void onPttPressed();
    void onPttReleased();
    void onCallEvent(int event, unsigned int src, unsigned int dst, bool group, float ber, const QString& text);
    void onRxAudio(const QVector<qint16>& pcm);
    void onMicAudio(const QVector<qint16>& pcm, float level);
    void onSpeakerLevel(float level);
    void onLinkLost();
    void onLog(const QString& text);
    void onGains();
    void onModeChanged();
    void onDmrOperation();

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void setConnected(bool connected);
    void setIndicator(QLabel* label, const QString& text, const QString& colour);

    QLineEdit*      host_;
    QSpinBox*       modem_;
    QComboBox*      mode_;
    QWidget*        nxBox_;
    QWidget*        dmrBox_;
    QWidget*        p25Box_;
    QLineEdit*      p25Nac_;
    QSpinBox*       p25Src_;
    QSpinBox*       p25Dst_;
    QCheckBox*      p25Group_;
    QWidget*        ysfBox_;
    QLineEdit*      ysfCall_;
    QLineEdit*      ysfDest_;
    QSpinBox*       ysfDgid_;
    QWidget*        dstarBox_;
    QLineEdit*      dstarCall_;
    QLineEdit*      dstarSuffix_;
    QLineEdit*      dstarUr_;
    QLineEdit*      dstarRpt1_;
    QLineEdit*      dstarRpt2_;
    QLineEdit*      dstarMsg_;
    QWidget*        m17Box_;
    QLineEdit*      m17Call_;
    QLineEdit*      m17Dest_;
    QSpinBox*       m17Can_;
    QComboBox*      m17Filter_;
    QWidget*        fmBox_;
    QDoubleSpinBox* fmTxTone_;
    QDoubleSpinBox* fmToneLevel_;
    QDoubleSpinBox* fmDtmfLevel_;
    QSpinBox*       fmDtmfHang_;
    QComboBox*      fmSquelch_;
    QDoubleSpinBox* fmRxTone_;
    QSpinBox*       fmDelay_;
    QDoubleSpinBox* fmSquelchLevel_;
    QCheckBox*      fmAuto_;
    QSpinBox*       dmrCc_;
    QSpinBox*       dmrSrc_;
    QSpinBox*       dmrDst_;
    QCheckBox*      dmrGroup_;
    QComboBox*      dmrOperation_;
    QComboBox*      dmrSlot_;
    QSpinBox*       dmrTiming_;
    QSpinBox*       ran_;
    QSpinBox*       src_;
    QSpinBox*       dst_;
    QCheckBox*      group_;
    QLineEdit*      ambeDev_;
    QComboBox*      ambeBaud_;
    QCheckBox*      noVocoder_;
    QCheckBox*      capture_;
    QCheckBox*      invertRx_;
    QSpinBox*       txLevel_;
    QSpinBox*       rxLevel_;
    QSpinBox*       ambeChunk_;
    QSpinBox*       ambeGap_;
    QComboBox*      layout_;
    QComboBox*      audioIn_;
    QComboBox*      audioOut_;
    QComboBox*      rate_;
    QSlider*        micGain_;
    QSlider*        spkGain_;
    QPushButton*    connect_;
    QPushButton*    release_;
    QPushButton*    ptt_;
    QPushButton*    dtmf_;
    QFrame*         dtmfPad_;
    bool            spaceDown_ = false;         /* the space bar is holding the push to talk button */
    QLabel*         rxLed_;
    QLabel*         txLed_;
    QLabel*         heard_;
    QLabel*         status_;
    QProgressBar*   micMeter_;
    QProgressBar*   spkMeter_;
    QPlainTextEdit* log_;

    AudioIO*         audio_;
    RadioController* radio_;
    bool             pttHeld_;
    bool             connected_;
};

#endif
