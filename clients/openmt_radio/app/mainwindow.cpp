#include "mainwindow.h"

#include "audioio.h"
#include "monitorwidget.h"
#include "radiocontroller.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include "../core/radio_engine.h"

static const char* const OFF_STYLE = "background:#444;color:#ccc;border-radius:4px;padding:6px;font-weight:bold;";

MainWindow::MainWindow(QWidget* parent) :
    QMainWindow(parent),
    host_(nullptr), modem_(nullptr), mode_(nullptr), nxBox_(nullptr), dmrBox_(nullptr), p25Box_(nullptr), p25Nac_(nullptr), p25Src_(nullptr), p25Dst_(nullptr), p25Group_(nullptr), ysfBox_(nullptr), ysfCall_(nullptr), ysfDest_(nullptr), ysfDgid_(nullptr), dstarBox_(nullptr), dstarCall_(nullptr), dstarSuffix_(nullptr), dstarUr_(nullptr), dstarRpt1_(nullptr), dstarRpt2_(nullptr), dstarMsg_(nullptr), m17Box_(nullptr), m17Call_(nullptr), m17Dest_(nullptr), m17Can_(nullptr), m17Filter_(nullptr), m17Meta_(nullptr), m17MsgTo_(nullptr), m17Msg_(nullptr), m17Send_(nullptr), fmBox_(nullptr), fmTxTone_(nullptr), fmToneLevel_(nullptr), fmDtmfLevel_(nullptr), fmDtmfHang_(nullptr), fmSquelch_(nullptr), fmRxTone_(nullptr), fmDelay_(nullptr), fmSquelchLevel_(nullptr), fmAuto_(nullptr), dmrCc_(nullptr),
    dmrSrc_(nullptr), dmrDst_(nullptr), dmrGroup_(nullptr), dmrOperation_(nullptr), dmrSlot_(nullptr), dmrTiming_(nullptr), ran_(nullptr), src_(nullptr), dst_(nullptr), group_(nullptr),
    ambeDev_(nullptr), ambeBaud_(nullptr), noVocoder_(nullptr), capture_(nullptr), invertRx_(nullptr), txLevel_(nullptr), rxLevel_(nullptr), spectrumPort_(nullptr), ambeChunk_(nullptr), ambeGap_(nullptr), layout_(nullptr), audioIn_(nullptr),
    audioOut_(nullptr), rate_(nullptr), micGain_(nullptr), spkGain_(nullptr), connect_(nullptr), release_(nullptr), ptt_(nullptr), dtmf_(nullptr), dtmfPad_(nullptr),
    rxLed_(nullptr), txLed_(nullptr), heard_(nullptr), status_(nullptr), micMeter_(nullptr), spkMeter_(nullptr),
    log_(nullptr),
    audio_(new AudioIO(this)),
    radio_(new RadioController(this)),
    pttHeld_(false),
    connected_(false)
{
    setWindowTitle("OpenMT Radio");
    buildUi();
    loadSettings();

    QObject::connect(radio_, SIGNAL(callEvent(int, unsigned int, unsigned int, bool, float, QString)),
                     this, SLOT(onCallEvent(int, unsigned int, unsigned int, bool, float, QString)));
    QObject::connect(radio_, SIGNAL(rxAudio(QVector<qint16>)), this, SLOT(onRxAudio(QVector<qint16>)));
    QObject::connect(radio_, SIGNAL(logLine(QString)), this, SLOT(onLog(QString)));
    QObject::connect(radio_, SIGNAL(linkLost()), this, SLOT(onLinkLost()));
    QObject::connect(audio_, SIGNAL(micAudio(QVector<qint16>, float)), this, SLOT(onMicAudio(QVector<qint16>, float)));
    QObject::connect(audio_, SIGNAL(playLevel(float)), this, SLOT(onSpeakerLevel(float)));

    openDongle();                       /* the dongle's port stays open from here until the program ends */
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
}

void MainWindow::buildUi()
{
    QWidget* central = new QWidget(this);
    QVBoxLayout* top = new QVBoxLayout(central);

    /* connection */
    QGroupBox* conn = new QGroupBox("Modem_Host", central);
    QFormLayout* cf = new QFormLayout(conn);
    host_ = new QLineEdit("127.0.0.1", conn);
    modem_ = new QSpinBox(conn);
    modem_->setRange(1, 10);
    mode_ = new QComboBox(conn);
    for (int i = (int)openmt::MODE_NXDN; i <= (int)openmt::MODE_FM; i++)
    {
        mode_->addItem(openmt::modeLabel((openmt::ModeId)i), i);
        if (!openmt::modeAvailable((openmt::ModeId)i))
        {
            /* greyed out: no engine for it in this version */
            QVariant none(0);
            mode_->setItemData(mode_->count() - 1, none, Qt::UserRole - 1);
            mode_->setItemText(mode_->count() - 1, QString("%1 (not yet)").arg(openmt::modeLabel((openmt::ModeId)i)));
        }
    }
    cf->addRow("Host", host_);
    cf->addRow("Modem number", modem_);
    cf->addRow("Mode", mode_);
    capture_ = new QCheckBox("Record received samples to ~/openmt_rx.raw", conn);
    capture_->setToolTip("Writes everything Modem_Host sends (16 bit, 24 kHz) to a file, for finding out why a signal is not decoded");
    cf->addRow("", capture_);
    invertRx_ = new QCheckBox("Invert the received signal", conn);
    invertRx_->setToolTip("Turns the polarity of the samples from Modem_Host round, if the signal is there but nothing decodes");
    cf->addRow("", invertRx_);
    txLevel_ = new QSpinBox(conn);
    txLevel_->setRange(0, 250);
    txLevel_->setValue(50);
    txLevel_->setToolTip("The transmit level sent to Modem_Host with the mode, like txLevel of the protocol hosts (default 50)");
    cf->addRow("Transmit level", txLevel_);
    rxLevel_ = new QSpinBox(conn);
    rxLevel_->setRange(1, 200);
    rxLevel_->setValue(50);
    rxLevel_->setToolTip("Scales the samples received from Modem_Host, like rxLevel of the protocol hosts (the samples are multiplied by this / 50; default 50)");
    cf->addRow("Receive level", rxLevel_);
    spectrumPort_ = new QSpinBox(conn);
    spectrumPort_->setRange(0, 65535);
    spectrumPort_->setValue(18050);
    spectrumPort_->setToolTip("UDP port of the HackRF bridge's RF spectrum, on the Host above (the Monitor tab asks for it while it is shown). "
                              "Only the HackRF bridge answers. 0 = do not ask");
    cf->addRow("RF spectrum port", spectrumPort_);

    /* NXDN */
    QGroupBox* nx = new QGroupBox("NXDN", central);
    QFormLayout* nf = new QFormLayout(nx);
    ran_ = new QSpinBox(nx);
    ran_->setRange(0, 63);
    src_ = new QSpinBox(nx);
    src_->setRange(1, 65535);
    dst_ = new QSpinBox(nx);
    dst_->setRange(1, 65535);
    group_ = new QCheckBox("Group call", nx);
    nf->addRow("RAN", ran_);
    nf->addRow("My unit id", src_);
    nf->addRow("Call to", dst_);
    nf->addRow("", group_);
    nxBox_ = nx;

    /* DMR */
    QGroupBox* dm = new QGroupBox("DMR", central);
    QFormLayout* df = new QFormLayout(dm);
    dmrOperation_ = new QComboBox(dm);
    dmrOperation_->addItem("Direct mode (simplex hotspot)", 0);
    dmrOperation_->addItem("Repeater or duplex hotspot", 1);
    dmrSlot_ = new QComboBox(dm);
    dmrSlot_->addItem("Time slot 1", 1);
    dmrSlot_->addItem("Time slot 2", 2);
    dmrTiming_ = new QSpinBox(dm);
    dmrTiming_->setRange(-200, 600);
    dmrTiming_->setSuffix(" ms");
    dmrTiming_->setToolTip("Only used when the modem sends no TX timing report (the SX1255 bridge does not): the delay from handing a "
                           "sample to Modem_Host until it is heard on the receive side. Change it in steps of 10 ms until the "
                           "repeater puts the call in the right time slot.");
    df->addRow("Operation", dmrOperation_);
    df->addRow("Slot (repeater)", dmrSlot_);
    df->addRow("TX slot timing", dmrTiming_);
    connect(dmrOperation_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { onDmrOperation(); });
    dmrCc_ = new QSpinBox(dm);
    dmrCc_->setRange(0, 15);
    dmrSrc_ = new QSpinBox(dm);
    dmrSrc_->setRange(1, 16777215);
    dmrDst_ = new QSpinBox(dm);
    dmrDst_->setRange(1, 16777215);
    dmrGroup_ = new QCheckBox("Group call (talk group)", dm);
    df->addRow("Color code", dmrCc_);
    df->addRow("My radio id", dmrSrc_);
    df->addRow("Call to", dmrDst_);
    df->addRow("", dmrGroup_);
    dmrBox_ = dm;

    /* P25 */
    QGroupBox* pg = new QGroupBox("P25", central);
    QFormLayout* pf = new QFormLayout(pg);
    p25Nac_ = new QLineEdit("293", pg);
    p25Nac_->setMaxLength(3);
    p25Nac_->setToolTip("Network access code, three hex digits (000 - FFF)");
    p25Src_ = new QSpinBox(pg);
    p25Src_->setRange(1, 16777215);
    p25Dst_ = new QSpinBox(pg);
    p25Dst_->setRange(1, 16777215);
    p25Group_ = new QCheckBox("Group call (talk group)", pg);
    pf->addRow("NAC (hex)", p25Nac_);
    pf->addRow("My radio id", p25Src_);
    pf->addRow("Call to", p25Dst_);
    pf->addRow("", p25Group_);
    p25Box_ = pg;

    /* YSF */
    QGroupBox* yg = new QGroupBox("YSF", central);
    QFormLayout* yf = new QFormLayout(yg);
    ysfCall_ = new QLineEdit("N0CALL", yg);
    ysfCall_->setMaxLength(10);
    ysfCall_->setToolTip("This radio's callsign (up to 10 characters)");
    ysfDest_ = new QLineEdit("CQCQCQ", yg);
    ysfDest_->setMaxLength(10);
    ysfDest_->setToolTip("Station called; CQCQCQ is a call to everyone");
    ysfDgid_ = new QSpinBox(yg);
    ysfDgid_->setRange(0, 99);
    ysfDgid_->setToolTip("Digital group id (0 = all)");
    yf->addRow("My callsign", ysfCall_);
    yf->addRow("Call to", ysfDest_);
    yf->addRow("DG-ID", ysfDgid_);
    ysfBox_ = yg;

    /* D-STAR */
    QGroupBox* dg = new QGroupBox("D-STAR", central);
    QFormLayout* dsf = new QFormLayout(dg);
    dstarCall_ = new QLineEdit("N0CALL", dg);
    dstarCall_->setMaxLength(8);
    dstarCall_->setToolTip("MY: this radio's callsign (up to 8 characters)");
    dstarSuffix_ = new QLineEdit(dg);
    dstarSuffix_->setMaxLength(4);
    dstarSuffix_->setToolTip("The four character suffix after MY (optional)");
    dstarUr_ = new QLineEdit("CQCQCQ", dg);
    dstarUr_->setMaxLength(8);
    dstarUr_->setToolTip("UR: station called; CQCQCQ is a call to everyone");
    dstarRpt1_ = new QLineEdit("N0CALL B", dg);
    dstarRpt1_->setMaxLength(8);
    dstarRpt1_->setToolTip("RPT1: the repeater or hotspot, callsign and module. Must be the callsign the host is set to");
    dstarRpt2_ = new QLineEdit("N0CALL G", dg);
    dstarRpt2_->setMaxLength(8);
    dstarRpt2_->setToolTip("RPT2: its gateway (callsign and G). Must be the callsign the host is set to");
    dstarMsg_ = new QLineEdit(dg);
    dstarMsg_->setMaxLength(20);
    dstarMsg_->setToolTip("The 20 character text message sent in the slow data");
    dsf->addRow("My callsign", dstarCall_);
    dsf->addRow("Suffix", dstarSuffix_);
    dsf->addRow("Call to (UR)", dstarUr_);
    dsf->addRow("RPT1", dstarRpt1_);
    dsf->addRow("RPT2", dstarRpt2_);
    dsf->addRow("Message", dstarMsg_);
    dstarBox_ = dg;

    /* M17 */
    QGroupBox* mg = new QGroupBox("M17", central);
    QFormLayout* m17f = new QFormLayout(mg);
    m17Call_ = new QLineEdit("N0CALL", mg);
    m17Call_->setMaxLength(9);
    m17Call_->setToolTip("The source callsign of this radio (up to 9 characters)");
    m17Dest_ = new QLineEdit("ALL", mg);
    m17Dest_->setMaxLength(9);
    m17Dest_->setToolTip("The destination: ALL calls everyone");
    m17Can_ = new QSpinBox(mg);
    m17Can_->setRange(0, 15);
    m17Can_->setToolTip("Channel access number (0 - 15); the host may ignore calls with another number");
    m17f->addRow("My callsign", m17Call_);
    m17f->addRow("Call to", m17Dest_);
    m17Filter_ = new QComboBox(mg);
    m17Filter_->addItem("Light (modem hardware)", 1);
    m17Filter_->addItem("Root raised cosine (clean signal)", 0);
    m17Filter_->setToolTip("The filter on the received samples. Light suits the output of a modem such as an ADF7021, which is "
                           "already filtered; root raised cosine is the host's own and tolerates more noise on a clean signal");
    m17f->addRow("CAN", m17Can_);
    m17f->addRow("Receive filter", m17Filter_);
    m17Meta_ = new QLineEdit(mg);
    m17Meta_->setMaxLength(52);
    m17Meta_->setPlaceholderText("text sent with every transmission");
    m17Meta_->setToolTip("Metadata text (up to 52 characters) sent in the link setup frame of every voice transmission, and shown by "
                         "radios that display it. It can be changed while connected: the next transmission carries the new text");
    m17f->addRow("Text with voice", m17Meta_);
    m17MsgTo_ = new QLineEdit("ALL", mg);
    m17MsgTo_->setMaxLength(9);
    m17MsgTo_->setToolTip("The callsign a text message is for (ALL for everyone)");
    m17Msg_ = new QLineEdit(mg);
    m17Msg_->setPlaceholderText("message, sent in packet mode");
    m17Msg_->setToolTip("A text message (SMS) sent as an M17 packet to the callsign above. Press Enter or Send. Up to 796 bytes");
    m17Send_ = new QPushButton("Send", mg);
    m17Send_->setFocusPolicy(Qt::NoFocus);
    m17Send_->setEnabled(false);
    m17Send_->setToolTip("Sends the message as an M17 packet (needs a connection)");
    QWidget* msgRow = new QWidget(mg);
    QHBoxLayout* msgLayout = new QHBoxLayout(msgRow);
    msgLayout->setContentsMargins(0, 0, 0, 0);
    msgLayout->addWidget(m17Msg_, 1);
    msgLayout->addWidget(m17Send_);
    m17f->addRow("Message to", m17MsgTo_);
    m17f->addRow("Message", msgRow);
    QObject::connect(m17Send_, SIGNAL(clicked()), this, SLOT(onM17Send()));
    QObject::connect(m17Msg_, SIGNAL(returnPressed()), this, SLOT(onM17Send()));
    QObject::connect(m17Meta_, SIGNAL(editingFinished()), this, SLOT(onM17Meta()));
    m17Box_ = mg;

    /* FM */
    QGroupBox* fg = new QGroupBox("FM", central);
    QFormLayout* fmf = new QFormLayout(fg);
    fmTxTone_ = new QDoubleSpinBox(fg);
    fmTxTone_->setRange(0.0, 300.0);
    fmTxTone_->setDecimals(1);
    fmTxTone_->setValue(88.5);
    fmTxTone_->setSuffix(" Hz");
    fmTxTone_->setToolTip("The CTCSS tone sent with the speech to open the repeater; 0 sends none");
    fmToneLevel_ = new QDoubleSpinBox(fg);
    fmToneLevel_->setRange(0.0, 20.0);
    fmToneLevel_->setDecimals(1);
    fmToneLevel_->setValue(1.5);
    fmToneLevel_->setSuffix(" %");
    fmToneLevel_->setToolTip("The level of the tone, in percent of full scale (1.5 is about 1 kHz of deviation on the SX1255 bridge)");
    fmDtmfLevel_ = new QDoubleSpinBox(fg);
    fmDtmfLevel_->setRange(0.1, 20.0);
    fmDtmfLevel_->setDecimals(1);
    fmDtmfLevel_->setValue(2.5);
    fmDtmfLevel_->setSuffix(" %");
    fmDtmfLevel_->setToolTip("The level of each of the two DTMF tones, in percent of full scale (2.5 is about 1.5 kHz each on the SX1255 bridge)");
    fmDtmfHang_ = new QSpinBox(fg);
    fmDtmfHang_->setRange(0, 10000);
    fmDtmfHang_->setSingleStep(100);
    fmDtmfHang_->setValue(2000);
    fmDtmfHang_->setSuffix(" ms");
    fmDtmfHang_->setToolTip("How long the transmitter stays keyed after the last DTMF digit, so a number typed at the keypad is one transmission");
    fmSquelch_ = new QComboBox(fg);
    fmSquelch_->addItem("Noise (a carrier opens it)", 0);
    fmSquelch_->addItem("Tone (CTCSS)", 1);
    fmSquelch_->addItem("Noise and tone", 2);
    fmSquelch_->addItem("Open (always on)", 3);
    fmSquelch_->setToolTip("What opens the receiver. Noise suits a repeater that sends no tone");
    fmRxTone_ = new QDoubleSpinBox(fg);
    fmRxTone_->setRange(0.0, 300.0);
    fmRxTone_->setDecimals(1);
    fmRxTone_->setSuffix(" Hz");
    fmRxTone_->setToolTip("The tone the receiver waits for with the tone squelch; 0 = none");
    fmDelay_ = new QSpinBox(fg);
    fmDelay_->setRange(0, 2000);
    fmDelay_->setSuffix(" ms");
    fmDelay_->setValue(400);
    fmDelay_->setToolTip("The tone alone before the speech: the repeater needs about 400 ms to know the tone, "
                         "and the start of the speech is lost with less");
    fmSquelchLevel_ = new QDoubleSpinBox(fg);
    fmSquelchLevel_->setRange(0.5, 60.0);
    fmSquelchLevel_->setDecimals(1);
    fmSquelchLevel_->setValue(8.0);
    fmSquelchLevel_->setSuffix(" %");
    fmSquelchLevel_->setToolTip("The noise squelch opens when the noise above 6 kHz is below this, and closes at 1.75 times it. "
                                "The log shows the noise level the receiver sees: set this between the level with and without a signal");
    fmAuto_ = new QCheckBox("Automatic receive level", fg);
    fmAuto_->setChecked(true);
    fmAuto_->setToolTip("Brings the received speech to a steady level; a discriminator can give very little");
    fmf->addRow("Transmit tone", fmTxTone_);
    fmf->addRow("Tone level", fmToneLevel_);
    fmf->addRow("DTMF level", fmDtmfLevel_);
    fmf->addRow("DTMF hang time", fmDtmfHang_);
    fmf->addRow("Squelch", fmSquelch_);
    fmf->addRow("Receive tone", fmRxTone_);
    fmf->addRow("Noise squelch", fmSquelchLevel_);
    fmf->addRow("", fmAuto_);
    fmf->addRow("Key-up delay", fmDelay_);
    fmBox_ = fg;

    /* vocoder */
    QGroupBox* vo = new QGroupBox("AMBE3000R dongle", central);
    QFormLayout* vf = new QFormLayout(vo);
    ambeDev_ = new QLineEdit("/dev/ttyUSB0", vo);
    ambeBaud_ = new QComboBox(vo);
    ambeBaud_->addItems(QStringList() << "230400" << "460800" << "921600");
    layout_ = new QComboBox(vo);
    layout_->addItem("Detect when connecting", -1);
    layout_->addItem("Air interface (49 + FEC)", 1);
    layout_->addItem("No FEC", 0);
    layout_->addItem("Coding", 2);
    ambeChunk_ = new QSpinBox(vo);
    ambeChunk_->setRange(0, 400);
    ambeChunk_->setSpecialValueText("whole packet");
    ambeGap_ = new QSpinBox(vo);
    ambeGap_->setRange(0, 10000);
    ambeGap_->setSuffix(" us");
    noVocoder_ = new QCheckBox("No dongle (link test, no speech)", vo);
    vf->addRow("Device", ambeDev_);
    vf->addRow("Baud rate", ambeBaud_);
    vf->addRow("Block layout", layout_);
    vf->addRow("Write chunk (bytes)", ambeChunk_);
    vf->addRow("Gap between chunks", ambeGap_);
    vf->addRow("", noVocoder_);

    /* sound */
    QGroupBox* so = new QGroupBox("Sound card", central);
    QFormLayout* sf = new QFormLayout(so);
    audioIn_ = new QComboBox(so);
    audioIn_->addItems(AudioIO::inputDevices());
    audioOut_ = new QComboBox(so);
    audioOut_->addItems(AudioIO::outputDevices());
    rate_ = new QComboBox(so);
    rate_->addItem("48000", 48000);
    rate_->addItem("16000", 16000);
    rate_->addItem("8000", 8000);
    micGain_ = new QSlider(Qt::Horizontal, so);
    micGain_->setRange(0, 400);
    micGain_->setValue(100);
    spkGain_ = new QSlider(Qt::Horizontal, so);
    spkGain_->setRange(0, 400);
    spkGain_->setValue(100);
    micMeter_ = new QProgressBar(so);
    micMeter_->setRange(0, 100);
    micMeter_->setTextVisible(false);
    spkMeter_ = new QProgressBar(so);
    spkMeter_->setRange(0, 100);
    spkMeter_->setTextVisible(false);
    sf->addRow("Microphone", audioIn_);
    sf->addRow("Speaker", audioOut_);
    sf->addRow("Rate (Hz)", rate_);
    sf->addRow("Mic gain %", micGain_);
    sf->addRow("", micMeter_);
    sf->addRow("Speaker gain %", spkGain_);
    sf->addRow("", spkMeter_);

    QHBoxLayout* settings = new QHBoxLayout;
    QVBoxLayout* left = new QVBoxLayout;
    left->addWidget(conn);
    left->addWidget(nx);
    left->addWidget(dm);
    left->addWidget(pg);
    left->addWidget(yg);
    left->addWidget(dg);
    left->addWidget(mg);
    left->addWidget(fg);
    QVBoxLayout* right = new QVBoxLayout;
    right->addWidget(vo);
    right->addWidget(so);
    settings->addLayout(left);
    settings->addLayout(right);
    top->addLayout(settings);

    /* operation */
    QHBoxLayout* op = new QHBoxLayout;
    connect_ = new QPushButton("Connect", central);
    release_ = new QPushButton("Release modem", central);
    connect_->setFocusPolicy(Qt::NoFocus);              /* the space bar is the push to talk key, it must never press these */
    release_->setFocusPolicy(Qt::NoFocus);
    release_->setToolTip("Tells Modem_Host to let go of this radio's mode if it is stuck holding it. Hosts of other protocols are not affected");
    release_->setEnabled(false);
    rxLed_ = new QLabel("RX", central);
    txLed_ = new QLabel("TX", central);
    rxLed_->setAlignment(Qt::AlignCenter);
    txLed_->setAlignment(Qt::AlignCenter);
    rxLed_->setStyleSheet(OFF_STYLE);
    txLed_->setStyleSheet(OFF_STYLE);
    ptt_ = new QPushButton("PUSH TO TALK  (hold, or hold space)", central);
    ptt_->setMinimumHeight(48);
    ptt_->setEnabled(false);
    ptt_->setFocusPolicy(Qt::NoFocus);
    op->addWidget(connect_);
    op->addWidget(release_);
    op->addWidget(rxLed_);
    op->addWidget(txLed_);
    dtmf_ = new QPushButton("DTMF", central);
    dtmf_->setCheckable(true);
    dtmf_->setMinimumHeight(48);
    dtmf_->setEnabled(false);
    dtmf_->setFocusPolicy(Qt::NoFocus);
    dtmf_->setToolTip("Shows a keypad that sends DTMF tones. Only for modes whose host listens for DTMF (FM)");
    op->addWidget(ptt_, 1);
    op->addWidget(dtmf_);

    /* the keypad: a panel on top of the window (not a window of its own, so the push to talk button and the keys of the
       window keep working), shown by the DTMF button */
    dtmfPad_ = new QFrame(central);
    dtmfPad_->setFrameShape(QFrame::StyledPanel);
    dtmfPad_->setAutoFillBackground(true);
    QGridLayout* pad = new QGridLayout(dtmfPad_);
    static const char KEYS[] = "123A456B789C*0#D";

    for (int i = 0; i < 16; i++)
    {
        QPushButton* k = new QPushButton(QString(QChar(KEYS[i])), dtmfPad_);

        k->setMinimumSize(56, 48);
        k->setFocusPolicy(Qt::NoFocus);
        k->setProperty("digit", QString(QChar(KEYS[i])));
        QObject::connect(k, SIGNAL(clicked()), this, SLOT(onDtmfKey()));
        pad->addWidget(k, i / 4, i % 4);
    }
    dtmfPad_->adjustSize();
    dtmfPad_->hide();
    QObject::connect(dtmf_, SIGNAL(toggled(bool)), this, SLOT(onDtmfToggle(bool)));
    qApp->installEventFilter(this);
    top->addLayout(op);

    heard_ = new QLabel("Last heard: -", central);
    status_ = new QLabel("Not connected", central);
    top->addWidget(heard_);
    top->addWidget(status_);

    log_ = new QPlainTextEdit(central);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setMinimumHeight(110);
    top->addWidget(log_, 1);

    /* the radio is one tab, the signal monitor the other (the space bar must keep working as push to talk, so the tabs take no focus) */
    QTabWidget* tabs = new QTabWidget(this);
    tabs->setFocusPolicy(Qt::NoFocus);
    tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    tabs->addTab(central, "Radio");
    tabs->addTab(new MonitorWidget(&radio_->monitor(), &radio_->spectrum(), &radio_->iq(), tabs), "Monitor");
    setCentralWidget(tabs);

    QObject::connect(mode_, SIGNAL(currentIndexChanged(int)), this, SLOT(onModeChanged()));
    QObject::connect(connect_, SIGNAL(clicked()), this, SLOT(onConnect()));
    QObject::connect(release_, SIGNAL(clicked()), this, SLOT(onRelease()));
    QObject::connect(ptt_, SIGNAL(pressed()), this, SLOT(onPttPressed()));
    QObject::connect(ptt_, SIGNAL(released()), this, SLOT(onPttReleased()));
    QObject::connect(micGain_, SIGNAL(valueChanged(int)), this, SLOT(onGains()));
    QObject::connect(spkGain_, SIGNAL(valueChanged(int)), this, SLOT(onGains()));
}

void MainWindow::loadSettings()
{
    QSettings s("OpenMT", "OpenMT Radio");

    host_->setText(s.value("host", "127.0.0.1").toString());
    modem_->setValue(s.value("modem", 1).toInt());
    ran_->setValue(s.value("ran", 1).toInt());
    src_->setValue(s.value("src", 1).toInt());
    dst_->setValue(s.value("dst", 9).toInt());
    group_->setChecked(s.value("group", true).toBool());
    dmrCc_->setValue(s.value("dmrColorCode", 1).toInt());
    dmrSrc_->setValue(s.value("dmrSrc", 1).toInt());
    dmrDst_->setValue(s.value("dmrDst", 9).toInt());
    dmrGroup_->setChecked(s.value("dmrGroup", true).toBool());
    dmrOperation_->setCurrentIndex(dmrOperation_->findData(s.value("dmrRepeater", 0).toInt()) < 0 ? 0 : dmrOperation_->findData(s.value("dmrRepeater", 0).toInt()));
    dmrSlot_->setCurrentIndex(dmrSlot_->findData(s.value("dmrSlot", 1).toInt()) < 0 ? 0 : dmrSlot_->findData(s.value("dmrSlot", 1).toInt()));
    dmrTiming_->setValue(s.value("dmrTiming", 100).toInt());
    p25Nac_->setText(s.value("p25Nac", "293").toString());
    p25Src_->setValue(s.value("p25Src", 1).toInt());
    p25Dst_->setValue(s.value("p25Dst", 9).toInt());
    p25Group_->setChecked(s.value("p25Group", true).toBool());
    ysfCall_->setText(s.value("ysfCall", "N0CALL").toString());
    ysfDest_->setText(s.value("ysfDest", "CQCQCQ").toString());
    ysfDgid_->setValue(s.value("ysfDgid", 0).toInt());
    dstarCall_->setText(s.value("dstarCall", "N0CALL").toString());
    dstarSuffix_->setText(s.value("dstarSuffix", "").toString());
    dstarUr_->setText(s.value("dstarUr", "CQCQCQ").toString());
    dstarRpt1_->setText(s.value("dstarRpt1", "N0CALL B").toString());
    dstarRpt2_->setText(s.value("dstarRpt2", "N0CALL G").toString());
    dstarMsg_->setText(s.value("dstarMsg", "").toString());
    m17Call_->setText(s.value("m17Call", "N0CALL").toString());
    m17Dest_->setText(s.value("m17Dest", "ALL").toString());
    m17Can_->setValue(s.value("m17Can", 0).toInt());
    m17Meta_->setText(s.value("m17Meta", "").toString());
    m17MsgTo_->setText(s.value("m17MsgTo", "ALL").toString());
    fmTxTone_->setValue(s.value("fmTxTone", 88.5).toDouble());
    fmToneLevel_->setValue(s.value("fmToneLevel", 1.5).toDouble());
    fmDtmfLevel_->setValue(s.value("fmDtmfLevel", 2.5).toDouble());
    fmDtmfHang_->setValue(s.value("fmDtmfHang", 2000).toInt());
    fmSquelch_->setCurrentIndex(fmSquelch_->findData(s.value("fmSquelch", 0).toInt()) < 0 ? 0 : fmSquelch_->findData(s.value("fmSquelch", 0).toInt()));
    fmRxTone_->setValue(s.value("fmRxTone", 0.0).toDouble());
    fmDelay_->setValue(s.value("fmDelay", 400).toInt());
    fmSquelchLevel_->setValue(s.value("fmSquelchLevel", 8.0).toDouble());
    fmAuto_->setChecked(s.value("fmAuto", true).toBool());
    m17Filter_->setCurrentIndex(m17Filter_->findData(s.value("m17Filter", 1).toInt()) < 0 ? 0 : m17Filter_->findData(s.value("m17Filter", 1).toInt()));
    int mi = mode_->findData(s.value("mode", 0).toInt());
    mode_->setCurrentIndex(mi < 0 ? 0 : mi);
    onModeChanged();
    ambeDev_->setText(s.value("ambeDevice", "/dev/ttyUSB0").toString());
    ambeBaud_->setCurrentText(s.value("ambeBaud", "460800").toString());
    noVocoder_->setChecked(s.value("noVocoder", false).toBool());
    capture_->setChecked(s.value("capture", false).toBool());
    invertRx_->setChecked(s.value("invertRx", false).toBool());
    txLevel_->setValue(s.value("txLevel", 50).toInt());
    rxLevel_->setValue(s.value("rxLevel", 50).toInt());
    spectrumPort_->setValue(s.value("spectrumPort", 18050).toInt());
    ambeChunk_->setValue(s.value("ambeChunk", 64).toInt());
    ambeGap_->setValue(s.value("ambeGap", 500).toInt());
    int li = layout_->findData(s.value("layout", -1).toInt());
    layout_->setCurrentIndex(li < 0 ? 0 : li);

    int ai = audioIn_->findText(s.value("audioIn").toString());
    if (ai >= 0)
        audioIn_->setCurrentIndex(ai);
    int ao = audioOut_->findText(s.value("audioOut").toString());
    if (ao >= 0)
        audioOut_->setCurrentIndex(ao);
    int ri = rate_->findData(s.value("rate", 48000).toInt());
    rate_->setCurrentIndex(ri < 0 ? 0 : ri);
    micGain_->setValue(s.value("micGain", 100).toInt());
    spkGain_->setValue(s.value("spkGain", 100).toInt());
}

void MainWindow::saveSettings()
{
    QSettings s("OpenMT", "OpenMT Radio");

    s.setValue("host", host_->text());
    s.setValue("modem", modem_->value());
    s.setValue("ran", ran_->value());
    s.setValue("src", src_->value());
    s.setValue("dst", dst_->value());
    s.setValue("group", group_->isChecked());
    s.setValue("dmrColorCode", dmrCc_->value());
    s.setValue("dmrSrc", dmrSrc_->value());
    s.setValue("dmrDst", dmrDst_->value());
    s.setValue("dmrGroup", dmrGroup_->isChecked());
    s.setValue("dmrRepeater", dmrOperation_->currentData().toInt());
    s.setValue("dmrSlot", dmrSlot_->currentData().toInt());
    s.setValue("dmrTiming", dmrTiming_->value());
    s.setValue("p25Nac", p25Nac_->text());
    s.setValue("p25Src", p25Src_->value());
    s.setValue("p25Dst", p25Dst_->value());
    s.setValue("p25Group", p25Group_->isChecked());
    s.setValue("ysfCall", ysfCall_->text());
    s.setValue("ysfDest", ysfDest_->text());
    s.setValue("ysfDgid", ysfDgid_->value());
    s.setValue("dstarCall", dstarCall_->text());
    s.setValue("dstarSuffix", dstarSuffix_->text());
    s.setValue("dstarUr", dstarUr_->text());
    s.setValue("dstarRpt1", dstarRpt1_->text());
    s.setValue("dstarRpt2", dstarRpt2_->text());
    s.setValue("dstarMsg", dstarMsg_->text());
    s.setValue("m17Call", m17Call_->text());
    s.setValue("m17Dest", m17Dest_->text());
    s.setValue("m17Can", m17Can_->value());
    s.setValue("m17Meta", m17Meta_->text());
    s.setValue("m17MsgTo", m17MsgTo_->text());
    s.setValue("fmTxTone", fmTxTone_->value());
    s.setValue("fmToneLevel", fmToneLevel_->value());
    s.setValue("fmDtmfLevel", fmDtmfLevel_->value());
    s.setValue("fmDtmfHang", fmDtmfHang_->value());
    s.setValue("fmSquelch", fmSquelch_->currentData().toInt());
    s.setValue("fmRxTone", fmRxTone_->value());
    s.setValue("fmDelay", fmDelay_->value());
    s.setValue("fmSquelchLevel", fmSquelchLevel_->value());
    s.setValue("fmAuto", fmAuto_->isChecked());
    s.setValue("m17Filter", m17Filter_->currentData().toInt());
    s.setValue("mode", mode_->currentData().toInt());
    s.setValue("ambeDevice", ambeDev_->text());
    s.setValue("ambeBaud", ambeBaud_->currentText());
    s.setValue("noVocoder", noVocoder_->isChecked());
    s.setValue("capture", capture_->isChecked());
    s.setValue("invertRx", invertRx_->isChecked());
    s.setValue("txLevel", txLevel_->value());
    s.setValue("rxLevel", rxLevel_->value());
    s.setValue("spectrumPort", spectrumPort_->value());
    s.setValue("ambeChunk", ambeChunk_->value());
    s.setValue("ambeGap", ambeGap_->value());
    s.setValue("layout", layout_->currentData().toInt());
    s.setValue("audioIn", audioIn_->currentText());
    s.setValue("audioOut", audioOut_->currentText());
    s.setValue("rate", rate_->currentData().toInt());
    s.setValue("micGain", micGain_->value());
    s.setValue("spkGain", spkGain_->value());
}

void MainWindow::setIndicator(QLabel* label, const QString& text, const QString& colour)
{
    label->setText(text);
    label->setStyleSheet(colour.isEmpty() ? QString(OFF_STYLE)
                                          : QString("background:%1;color:white;border-radius:4px;padding:6px;font-weight:bold;").arg(colour));
}

void MainWindow::setConnected(bool connected)
{
    connected_ = connected;
    connect_->setText(connected ? "Disconnect" : "Connect");
    ptt_->setEnabled(connected);
    release_->setEnabled(connected);
    dtmf_->setEnabled(connected && radio_->dtmfSupported());
    if (!dtmf_->isEnabled())
        dtmf_->setChecked(false);
    m17Send_->setEnabled(connected && radio_->messagesSupported());

    /* the settings cannot change while the radio runs */
    host_->setEnabled(!connected);
    modem_->setEnabled(!connected);
    mode_->setEnabled(!connected);
    ran_->setEnabled(!connected);
    src_->setEnabled(!connected);
    dst_->setEnabled(!connected);
    group_->setEnabled(!connected);
    dmrCc_->setEnabled(!connected);
    dmrSrc_->setEnabled(!connected);
    dmrDst_->setEnabled(!connected);
    dmrGroup_->setEnabled(!connected);
    dmrOperation_->setEnabled(!connected);
    dmrSlot_->setEnabled(!connected);
    dmrTiming_->setEnabled(!connected);
    p25Nac_->setEnabled(!connected);
    p25Src_->setEnabled(!connected);
    p25Dst_->setEnabled(!connected);
    p25Group_->setEnabled(!connected);
    ysfCall_->setEnabled(!connected);
    ysfDest_->setEnabled(!connected);
    ysfDgid_->setEnabled(!connected);
    dstarCall_->setEnabled(!connected);
    dstarSuffix_->setEnabled(!connected);
    dstarUr_->setEnabled(!connected);
    dstarRpt1_->setEnabled(!connected);
    dstarRpt2_->setEnabled(!connected);
    dstarMsg_->setEnabled(!connected);
    m17Call_->setEnabled(!connected);
    m17Dest_->setEnabled(!connected);
    m17Can_->setEnabled(!connected);
    fmTxTone_->setEnabled(!connected);
    fmToneLevel_->setEnabled(!connected);
    fmDtmfLevel_->setEnabled(!connected);
    fmDtmfHang_->setEnabled(!connected);
    fmSquelch_->setEnabled(!connected);
    fmRxTone_->setEnabled(!connected);
    fmDelay_->setEnabled(!connected);
    fmSquelchLevel_->setEnabled(!connected);
    fmAuto_->setEnabled(!connected);
    m17Filter_->setEnabled(!connected);
    ambeDev_->setEnabled(!connected);
    ambeBaud_->setEnabled(!connected);
    noVocoder_->setEnabled(!connected);
    capture_->setEnabled(!connected);
    invertRx_->setEnabled(!connected);
    txLevel_->setEnabled(!connected);
    rxLevel_->setEnabled(!connected);
    spectrumPort_->setEnabled(!connected);
    ambeChunk_->setEnabled(!connected);
    ambeGap_->setEnabled(!connected);
    layout_->setEnabled(!connected);
    audioIn_->setEnabled(!connected);
    audioOut_->setEnabled(!connected);
    rate_->setEnabled(!connected);

    if (!connected)
    {
        setIndicator(rxLed_, "RX", QString());
        setIndicator(txLed_, "TX", QString());
        micMeter_->setValue(0);
        spkMeter_->setValue(0);
    }
}

void MainWindow::onConnect()
{
    if (connected_)
    {
        onPttReleased();
        radio_->stop();
        audio_->stop();
        setConnected(false);
        status_->setText("Not connected");
        return;
    }

    openmt::ModeId mode = (openmt::ModeId)mode_->currentData().toInt();
    if (!openmt::modeAvailable(mode))
    {
        status_->setText(QString("%1 is not available in this version").arg(openmt::modeLabel(mode)));
        return;
    }

    openmt::EngineSettings es;
    es.mode = mode;
    es.link.host = host_->text().toStdString();
    es.link.modem = (unsigned int)modem_->value();
    es.link.txLevel = (uint8_t)txLevel_->value();
    es.nxdn.ran = (unsigned int)ran_->value();
    es.nxdn.srcId = (unsigned int)src_->value();
    es.nxdn.dstId = (unsigned int)dst_->value();
    es.nxdn.group = group_->isChecked();
    es.nxdn.layout = layout_->currentData().toInt();
    es.dmr.colorCode = (unsigned int)dmrCc_->value();
    es.dmr.srcId = (unsigned int)dmrSrc_->value();
    es.dmr.dstId = (unsigned int)dmrDst_->value();
    es.dmr.group = dmrGroup_->isChecked();
    es.dmr.repeater = dmrOperation_->currentData().toInt() == 1;
    es.dmr.slot = (unsigned int)dmrSlot_->currentData().toInt();
    es.dmr.txTimingMs = dmrTiming_->value();
    es.dmr.layout = es.nxdn.layout == 0 ? -1 : es.nxdn.layout;      /* "no FEC" is an NXDN arrangement only */
    es.ambeDevice = ambeDev_->text().trimmed().toStdString();
    es.ambeBaud = ambeBaud_->currentText().toUInt();
    es.noVocoder = noVocoder_->isChecked();
    {
        bool nacOk = false;
        unsigned int nac = p25Nac_->text().toUInt(&nacOk, 16);

        es.p25.nac = nacOk && nac <= 0xFFFU ? nac : 0x293U;
    }
    es.p25.srcId = (unsigned int)p25Src_->value();
    es.p25.dstId = (unsigned int)p25Dst_->value();
    es.p25.group = p25Group_->isChecked();
    es.ysf.callsign = ysfCall_->text().trimmed().toUpper().toStdString();
    if (es.ysf.callsign.empty())
        es.ysf.callsign = "N0CALL";
    es.ysf.dest = ysfDest_->text().trimmed().toUpper().toStdString();
    if (es.ysf.dest.empty())
        es.ysf.dest = "CQCQCQ";
    es.ysf.dgid = (unsigned int)ysfDgid_->value();
    es.ysf.layout = es.nxdn.layout;                                 /* the chip's arrangement of the 49 bits is the NXDN one */
    es.dstar.callsign = dstarCall_->text().trimmed().toUpper().toStdString();
    if (es.dstar.callsign.empty())
        es.dstar.callsign = "N0CALL";
    es.dstar.suffix = dstarSuffix_->text().toUpper().toStdString();
    es.dstar.ur = dstarUr_->text().toUpper().toStdString();
    if (es.dstar.ur.empty())
        es.dstar.ur = "CQCQCQ";
    es.dstar.rpt1 = dstarRpt1_->text().toUpper().toStdString();
    es.dstar.rpt2 = dstarRpt2_->text().toUpper().toStdString();
    es.dstar.message = dstarMsg_->text().toStdString();                /* D-STAR finds an upside down signal by itself */
    es.m17.callsign = m17Call_->text().trimmed().toUpper().toStdString();
    if (es.m17.callsign.empty())
        es.m17.callsign = "N0CALL";
    es.m17.dest = m17Dest_->text().trimmed().toUpper().toStdString();
    if (es.m17.dest.empty())
        es.m17.dest = "ALL";
    es.m17.can = (unsigned int)m17Can_->value();
    {
        QByteArray meta = m17Meta_->text().toUtf8();

        es.m17.metaText = std::string(meta.constData(), (size_t)meta.size());
    }
    es.m17.rxFilter = m17Filter_->currentData().toInt();
    es.fm.txCtcss = (float)fmTxTone_->value();
    es.fm.txToneLevel = (float)fmToneLevel_->value();
    es.fm.dtmfLevel = (float)fmDtmfLevel_->value();
    es.fm.dtmfHangMs = (unsigned int)fmDtmfHang_->value();
    es.fm.squelch = (unsigned int)fmSquelch_->currentData().toInt();
    es.fm.rxCtcss = (float)fmRxTone_->value();
    es.fm.txDelayMs = (unsigned int)fmDelay_->value();
    es.fm.noiseOpen = (float)fmSquelchLevel_->value();
    es.fm.noiseClose = es.fm.noiseOpen * 1.75F;
    es.fm.autoLevel = fmAuto_->isChecked();
    float level = (float)rxLevel_->value() / 50.0F;                    /* as the protocol hosts do with rxLevel */

    es.nxdn.rxGain = es.dmr.rxGain = es.p25.rxGain = es.ysf.rxGain = es.m17.rxGain = invertRx_->isChecked() ? -level : level;
    es.dstar.rxGain = es.fm.rxGain = level;                             /* D-STAR finds the polarity itself, FM audio has none */
    es.spectrumPort = (unsigned int)spectrumPort_->value();
    es.rxCapture = capture_->isChecked() ? (QDir::homePath() + "/openmt_rx.raw").toStdString() : std::string();
    es.ambeChunk = ambeChunk_->value();
    es.ambeGapUs = ambeGap_->value();

    QString error;
    log_->clear();                                      /* a new connection starts with an empty log */
    openDongle();                                       /* normally open already; this opens it if it was not there at the start */
    if (radio_->dongleOpen())
        onLog("AMBE3000R: the serial port is open");
    status_->setText("Connecting...");

    if (!radio_->start(es, error))
    {
        status_->setText("Could not start: " + error);
        onLog("start failed: " + error);
        return;
    }

    if (!audio_->start(audioIn_->currentText(), audioOut_->currentText(), (unsigned int)rate_->currentData().toInt(), error))
    {
        radio_->stop();
        status_->setText("Sound card: " + error);
        onLog("sound card failed: " + error);
        return;
    }

    onGains();
    saveSettings();
    setConnected(true);
    status_->setText(QString("Connected to %1, modem %2 - %3")
                         .arg(host_->text()).arg(modem_->value()).arg(radio_->vocoderDescription()));
}

void MainWindow::onModeChanged()
{
    openmt::ModeId mode = (openmt::ModeId)mode_->currentData().toInt();

    nxBox_->setVisible(mode == openmt::MODE_NXDN || !openmt::modeAvailable(mode));
    dmrBox_->setVisible(mode == openmt::MODE_DMR);
    p25Box_->setVisible(mode == openmt::MODE_P25);
    ysfBox_->setVisible(mode == openmt::MODE_YSF);
    dstarBox_->setVisible(mode == openmt::MODE_DSTAR);
    m17Box_->setVisible(mode == openmt::MODE_M17);
    fmBox_->setVisible(mode == openmt::MODE_FM);
    onDmrOperation();
}

void MainWindow::onDmrOperation()
{
    bool repeater = dmrOperation_->currentData().toInt() == 1;

    dmrSlot_->setVisible(repeater);
    dmrTiming_->setVisible(repeater);

    /* the labels of the form rows go with their fields */
    QFormLayout* f = qobject_cast<QFormLayout*>(dmrBox_->layout());

    if (f)
    {
        QWidget* l1 = f->labelForField(dmrSlot_);
        QWidget* l2 = f->labelForField(dmrTiming_);

        if (l1)
            l1->setVisible(repeater);
        if (l2)
            l2->setVisible(repeater);
    }
}

void MainWindow::onGains()
{
    audio_->setMicGain((float)micGain_->value() / 100.0F);
    audio_->setSpeakerGain((float)spkGain_->value() / 100.0F);
}

void MainWindow::onRelease()
{
    if (!radio_->releaseModem())
        onLog("release: not connected");
}

void MainWindow::onPttPressed()
{
    if (!connected_ || pttHeld_)
        return;
    pttHeld_ = true;
    radio_->ptt(true);
}

/* under the DTMF button, its right edge at the right edge of the button, kept inside the window */
void MainWindow::placeDtmfPad()
{
    QWidget* parent = dtmfPad_->parentWidget();
    QPoint p = dtmf_->mapTo(parent, QPoint(dtmf_->width() - dtmfPad_->width(), dtmf_->height() + 4));

    if (p.x() < 0)
        p.setX(0);
    dtmfPad_->move(p);
    dtmfPad_->raise();
}

/* Opens the AMBE3000R if it is not open (or its port or speed has been changed).  It is never closed again before the program
   ends, whether or not the mode in use needs it: opening the port can reset the dongle and it takes seconds to come back. */
void MainWindow::openDongle()
{
    if (noVocoder_->isChecked() || ambeDev_->text().trimmed().isEmpty())
        return;

    QString error;

    if (!radio_->openDongle(ambeDev_->text().trimmed(), ambeBaud_->currentText().toUInt(), ambeChunk_->value(), ambeGap_->value(), error))
        onLog("AMBE3000R: " + error + " (it is tried again when you press Connect)");
}

void MainWindow::onDtmfToggle(bool on)
{
    if (!on)
    {
        dtmfPad_->hide();
        return;
    }
    dtmfPad_->adjustSize();
    dtmfPad_->show();
    placeDtmfPad();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (dtmfPad_ != nullptr && dtmfPad_->isVisible())
        placeDtmfPad();
}

/* The space bar is the push to talk key.  It is taken before the widget that has the focus gets it (a button or a check
   box would otherwise be pressed by it), except when text is being typed. */
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) && QApplication::activeWindow() == this)
    {
        QKeyEvent* key = static_cast<QKeyEvent*>(event);

        if (key->key() == Qt::Key_Space)
        {
            if (event->type() == QEvent::KeyRelease)
            {
                if (spaceDown_ && !key->isAutoRepeat())
                {
                    spaceDown_ = false;
                    ptt_->setDown(false);
                    onPttReleased();
                    return true;
                }
                if (spaceDown_)
                    return true;
            }
            else
            {
                QWidget* focus = QApplication::focusWidget();
                bool typing = focus != nullptr && (qobject_cast<QLineEdit*>(focus) != nullptr ||
                                                   qobject_cast<QAbstractSpinBox*>(focus) != nullptr ||
                                                   qobject_cast<QPlainTextEdit*>(focus) != nullptr ||
                                                   (qobject_cast<QComboBox*>(focus) != nullptr &&
                                                    qobject_cast<QComboBox*>(focus)->isEditable()));

                if (spaceDown_)
                    return true;                /* a repeat of the key that is down */
                if (connected_ && !typing)
                {
                    spaceDown_ = true;
                    ptt_->setDown(true);
                    onPttPressed();
                    return true;
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onDtmfKey()
{
    QPushButton* k = qobject_cast<QPushButton*>(sender());

    if (k == nullptr || !connected_)
        return;

    QString digit = k->property("digit").toString();

    if (!digit.isEmpty() && !radio_->sendDtmf(digit.at(0).toLatin1()))
        onLog("DTMF " + digit + " was not sent (the radio is not connected, or there are too many digits waiting)");
}

void MainWindow::onPttReleased()
{
    if (!pttHeld_)
        return;
    pttHeld_ = false;
    radio_->ptt(false);
}

void MainWindow::onCallEvent(int event, unsigned int src, unsigned int dst, bool group, float ber, const QString& text)
{
    switch (event)
    {
    case openmt::CALL_RX_START:
        setIndicator(rxLed_, "RX", "#2a8a2a");
        if (src == 0U && dst == 0U && !text.isEmpty())
            heard_->setText(QString("Last heard: %1").arg(text));
        break;
    case openmt::CALL_RX_INFO:
        if (src == 0U && dst == 0U && !text.isEmpty())              /* YSF, D-STAR, M17: callsigns */
            heard_->setText(QString("Last heard: %1").arg(text));
        else
            heard_->setText(QString("Last heard: %1 -> %2%3  %4").arg(src).arg(group ? "TG " : "").arg(dst).arg(text));
        break;
    case openmt::CALL_RX_END:
    case openmt::CALL_RX_LOST:
        setIndicator(rxLed_, "RX", QString());
        if (src == 0U && dst == 0U)
            heard_->setText(QString("Last heard: %1  BER %2%  %3 (%4)")
                                .arg(text.isEmpty() ? QString("unknown") : text).arg((double)ber, 0, 'f', 1)
                                .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
                                .arg(event == openmt::CALL_RX_END ? "end" : "lost"));
        else
            heard_->setText(QString("Last heard: %1 -> %2%3  BER %4%  %5 (%6)")
                                .arg(src).arg(group ? "TG " : "").arg(dst).arg((double)ber, 0, 'f', 1)
                                .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
                                .arg(event == openmt::CALL_RX_END ? "end" : "lost"));
        break;
    case openmt::CALL_TX_START:
        setIndicator(txLed_, "TX", "#b02020");
        break;
    case openmt::CALL_TX_END:
        setIndicator(txLed_, "TX", QString());
        break;
    default:
        break;
    }
}

void MainWindow::onRxAudio(const QVector<qint16>& pcm)
{
    audio_->play(reinterpret_cast<const int16_t*>(pcm.constData()), (size_t)pcm.size());
}

void MainWindow::onMicAudio(const QVector<qint16>& pcm, float level)
{
    micMeter_->setValue((int)(level * 100.0F));
    if (pttHeld_)
        radio_->pushMic(pcm);
}

void MainWindow::onSpeakerLevel(float level)
{
    spkMeter_->setValue((int)(level * 100.0F));
}

void MainWindow::onLinkLost()
{
    onPttReleased();
    radio_->stop();
    audio_->stop();
    setConnected(false);
    status_->setText("Connection to Modem_Host lost");
    onLog("connection to Modem_Host lost");
}

void MainWindow::onLog(const QString& text)
{
    log_->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss  ") + text);
}

void MainWindow::onM17Send()
{
    if (!connected_)
        return;

    QString text = m17Msg_->text().trimmed();

    if (text.isEmpty())
        return;

    QString error;

    if (radio_->sendMessage(m17MsgTo_->text().trimmed().toUpper(), text, error))
        m17Msg_->clear();                       /* it is queued: the log says when it has been sent */
    else
        onLog("M17: the message was not sent: " + error);
}

void MainWindow::onM17Meta()
{
    if (connected_)
        radio_->setMetaText(m17Meta_->text());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    radio_->stop();
    audio_->stop();
    event->accept();
}
