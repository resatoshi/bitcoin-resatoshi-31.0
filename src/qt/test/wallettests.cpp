// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/wallettests.h>
#include <qt/test/util.h>

#include <wallet/coincontrol.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <netmessagemaker.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/cpuminer.h>
#include <qt/bootstrapmanager.h>
#include <qt/cpuminerhash.h>
#include <qt/minerdashboard.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/receivecoinsdialog.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <rpc/server.h>
#include <script/solver.h>
#include <test/util/setup_common.h>
#include <test/util/net.h>
#include <validation.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <core_io.h>

#include <chrono>
#include <memory>
#include <latch>
#include <limits>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QClipboard>
#include <QObject>
#include <QPushButton>
#include <QTimer>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QListView>
#include <QDialogButtonBox>

using wallet::AddWallet;
using wallet::CWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WalletRescanReserver;

namespace
{
//! Press "Yes" or "Cancel" buttons in modal send confirmation dialog.
void ConfirmSend(QString* text = nullptr, QMessageBox::StandardButton confirm_type = QMessageBox::Yes)
{
    QTimer::singleShot(0, [text, confirm_type]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("SendConfirmationDialog")) {
                SendConfirmationDialog* dialog = qobject_cast<SendConfirmationDialog*>(widget);
                if (text) *text = dialog->text();
                QAbstractButton* button = dialog->button(confirm_type);
                button->setEnabled(true);
                button->click();
            }
        }
    });
}

//! Send coins to address and return txid.
Txid SendCoins(CWallet& wallet, SendCoinsDialog& sendCoinsDialog, const CTxDestination& address, CAmount amount, bool rbf,
                  QMessageBox::StandardButton confirm_type = QMessageBox::Yes)
{
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<BitcoinAmountField*>("payAmount")->setValue(amount);
    sendCoinsDialog.findChild<QFrame*>("frameFee")
        ->findChild<QFrame*>("frameFeeSelection")
        ->findChild<QCheckBox*>("optInRBF")
        ->setCheckState(rbf ? Qt::Checked : Qt::Unchecked);
    Txid txid;
    boost::signals2::scoped_connection c(wallet.NotifyTransactionChanged.connect([&txid](const Txid& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));
    ConfirmSend(/*text=*/nullptr, confirm_type);
    bool invoked = QMetaObject::invokeMethod(&sendCoinsDialog, "sendButtonClicked", Q_ARG(bool, false));
    assert(invoked);
    return txid;
}

//! Find index of txid in transaction list.
QModelIndex FindTx(const QAbstractItemModel& model, const Txid& txid)
{
    QString hash = QString::fromStdString(txid.ToString());
    int rows = model.rowCount({});
    for (int row = 0; row < rows; ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            return index;
        }
    }
    return {};
}

//! Invoke bumpfee on txid and check results.
void BumpFee(TransactionView& view, const Txid& txid, bool expectDisabled, std::string expectError, bool cancel)
{
    QTableView* table = view.findChild<QTableView*>("transactionView");
    QModelIndex index = FindTx(*table->selectionModel()->model(), txid);
    QVERIFY2(index.isValid(), "Could not find BumpFee txid");

    // Select row in table, invoke context menu, and make sure bumpfee action is
    // enabled or disabled as expected.
    QAction* action = view.findChild<QAction*>("bumpFeeAction");
    table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    action->setEnabled(expectDisabled);
    table->customContextMenuRequested({});
    QCOMPARE(action->isEnabled(), !expectDisabled);

    action->setEnabled(true);
    QString text;
    if (expectError.empty()) {
        ConfirmSend(&text, cancel ? QMessageBox::Cancel : QMessageBox::Yes);
    } else {
        ConfirmMessage(&text, 0ms);
    }
    action->trigger();
    QVERIFY(text.indexOf(QString::fromStdString(expectError)) != -1);
}

void CompareBalance(WalletModel& walletModel, CAmount expected_balance, QLabel* balance_label_to_check)
{
    BitcoinUnit unit = walletModel.getOptionsModel()->getDisplayUnit();
    QString balanceComparison = BitcoinUnits::formatWithUnit(unit, expected_balance, false, BitcoinUnits::SeparatorStyle::ALWAYS);
    QCOMPARE(balance_label_to_check->text().trimmed(), balanceComparison);
}

// Verify the 'useAvailableBalance' functionality. With and without manually selected coins.
// Case 1: No coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the total available balance
// Case 2: With coin control selected coins.
// 'useAvailableBalance' should fill the amount edit box with the sum of the selected coins values.
void VerifyUseAvailableBalance(SendCoinsDialog& sendCoinsDialog, const WalletModel& walletModel)
{
    // Verify first entry amount and "useAvailableBalance" button
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    QVERIFY(entries->count() == 1); // only one entry
    SendCoinsEntry* send_entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QVERIFY(send_entry->getValue().amount == 0);
    // Now click "useAvailableBalance", check updated balance (the entire wallet balance should be set)
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == walletModel.getCachedBalance().balance);

    // Now manually select two coins and click on "useAvailableBalance". Then check updated balance
    // (only the sum of the selected coins should be set).
    int COINS_TO_SELECT = 2;
    auto coins = walletModel.wallet().listCoins();
    CAmount sum_selected_coins = 0;
    int selected = 0;
    QVERIFY(coins.size() == 1); // context check, coins received only on one destination
    for (const auto& [outpoint, tx_out] : coins.begin()->second) {
        sendCoinsDialog.getCoinControl()->Select(outpoint);
        sum_selected_coins += tx_out.txout.nValue;
        if (++selected == COINS_TO_SELECT) break;
    }
    QVERIFY(selected == COINS_TO_SELECT);

    // Now that we have 2 coins selected, "useAvailableBalance" should update the balance label only with
    // the sum of them.
    Q_EMIT send_entry->useAvailableBalance(send_entry);
    QVERIFY(send_entry->getValue().amount == sum_selected_coins);
}

void SyncUpWallet(const std::shared_ptr<CWallet>& wallet, interfaces::Node& node)
{
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/true, /*save_progress=*/false);
    QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    QCOMPARE(result.last_scanned_block, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    QVERIFY(result.last_failed_block.IsNull());
}

std::shared_ptr<CWallet> SetupDescriptorsWallet(interfaces::Node& node, TestChain100Setup& test, bool watch_only = false)
{
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    if (watch_only) {
        wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    } else {
        wallet->SetupDescriptorScriptPubKeyMans();
    }

    // Add the coinbase key
    FlatSigningProvider provider;
    std::string error;
    std::string key_str;
    if (watch_only) {
        key_str = HexStr(test.coinbaseKey.GetPubKey());
    } else {
        key_str = EncodeSecret(test.coinbaseKey);
    }
    auto descs = Parse("combo(" + key_str + ")", provider, error, /* require_checksum=*/ false);
    assert(!descs.empty());
    assert(descs.size() == 1);
    auto& desc = descs.at(0);
    WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    Assert(wallet->AddWalletDescriptor(w_desc, provider, "", false));
    const PKHash dest{test.coinbaseKey.GetPubKey()};
    wallet->SetAddressBook(dest, "", wallet::AddressPurpose::RECEIVE);
    wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    SyncUpWallet(wallet, node);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

struct MiniGUI {
public:
    SendCoinsDialog sendCoinsDialog;
    TransactionView transactionView;
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;

    MiniGUI(interfaces::Node& node, const PlatformStyle* platformStyle) : sendCoinsDialog(platformStyle), transactionView(platformStyle), optionsModel(node) {
        bilingual_str error;
        QVERIFY(optionsModel.Init(error));
        clientModel = std::make_unique<ClientModel>(node, &optionsModel);
    }

    void initModelForWallet(interfaces::Node& node, const std::shared_ptr<CWallet>& wallet, const PlatformStyle* platformStyle)
    {
        WalletContext& context = *node.walletLoader().context();
        AddWallet(context, wallet);
        walletModel = std::make_unique<WalletModel>(interfaces::MakeWallet(context, wallet), *clientModel, platformStyle);
        RemoveWallet(context, wallet, /* load_on_start= */ std::nullopt);
        sendCoinsDialog.setModel(walletModel.get());
        transactionView.setModel(walletModel.get());
    }

};

//! Simple qt wallet tests.
//
// Test widgets can be debugged interactively calling show() on them and
// manually running the event loop, e.g.:
//
//     sendCoinsDialog.show();
//     QEventLoop().exec();
//
// This also requires overriding the default minimal Qt platform:
//
//     QT_QPA_PLATFORM=xcb     build/bin/test_bitcoin-qt  # Linux
//     QT_QPA_PLATFORM=windows build/bin/test_bitcoin-qt  # Windows
//     QT_QPA_PLATFORM=cocoa   build/bin/test_bitcoin-qt  # macOS
void TestGUI(interfaces::Node& node, const std::shared_ptr<CWallet>& wallet)
{
    // Create widgets for sending coins and listing transactions.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(node, platformStyle.get());
    mini_gui.initModelForWallet(node, wallet, platformStyle.get());
    WalletModel& walletModel = *mini_gui.walletModel;
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;
    TransactionView& transactionView = mini_gui.transactionView;
    QVERIFY(walletModel.wallet().getInitialFallbackFee(1000) >= 1000);

    // Renewal first creates a non-broadcasting preview on the single-screen
    // dashboard. It must not require an unlock at this stage.
    MinerDashboard dashboard(&walletModel, platformStyle.get());
    dashboard.setClientModel(mini_gui.clientModel.get());
    qApp->processEvents();
    auto* renew_table = dashboard.findChild<QTableWidget*>("renewUtxosTable");
    auto* select_all = dashboard.findChild<QPushButton*>("selectAllRenewable");
    auto* preview = dashboard.findChild<QPushButton*>("renewUtxosButton");
    QVERIFY(renew_table);
    QVERIFY(select_all);
    QVERIFY(preview);
    QVERIFY(renew_table->rowCount() > 0);
    select_all->click();
    QVERIFY(preview->isEnabled());
    preview->click();
    QVERIFY(dashboard.findChild<QLabel*>("renewDestination")->text().contains("New address"));
    QVERIFY(dashboard.findChild<QLabel*>("renewSummary")->text().contains("fallback", Qt::CaseInsensitive));
    QVERIFY(dashboard.findChild<QLabel*>("renewStatus")->text().contains("Review", Qt::CaseInsensitive));

    // Update walletModel cached balance which will trigger an update for the 'labelBalance' QLabel.
    walletModel.pollBalanceChanged();
    // Check balance in send dialog
    CompareBalance(walletModel, walletModel.wallet().getBalance(), sendCoinsDialog.findChild<QLabel*>("labelBalance"));

    // Check 'UseAvailableBalance' functionality
    VerifyUseAvailableBalance(sendCoinsDialog, walletModel);

    // Send two transactions, and verify they are added to transaction list.
    TransactionTableModel* transactionTableModel = walletModel.getTransactionTableModel();
    QCOMPARE(transactionTableModel->rowCount({}), 105);
    Txid txid1 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 5 * COIN, /*rbf=*/false);
    Txid txid2 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 10 * COIN, /*rbf=*/true);
    // Transaction table model updates on a QueuedConnection, so process events to ensure it's updated.
    qApp->processEvents();
    QCOMPARE(transactionTableModel->rowCount({}), 107);
    QVERIFY(FindTx(*transactionTableModel, txid1).isValid());
    QVERIFY(FindTx(*transactionTableModel, txid2).isValid());

    // Call bumpfee. Test canceled fullrbf bump, canceled bip-125-rbf bump, passing bump, and then failing bump.
    BumpFee(transactionView, txid1, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/true);
    BumpFee(transactionView, txid2, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/true);
    BumpFee(transactionView, txid2, /*expectDisabled=*/false, /*expectError=*/{}, /*cancel=*/false);
    BumpFee(transactionView, txid2, /*expectDisabled=*/true, /*expectError=*/"already bumped", /*cancel=*/false);

    // Check current balance on OverviewPage
    OverviewPage overviewPage(platformStyle.get());
    overviewPage.setWalletModel(&walletModel);
    walletModel.pollBalanceChanged(); // Manual balance polling update
    CompareBalance(walletModel, walletModel.wallet().getBalance(), overviewPage.findChild<QLabel*>("labelBalance"));

    // Check Request Payment button
    ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
    receiveCoinsDialog.setModel(&walletModel);
    RecentRequestsTableModel* requestTableModel = walletModel.getRecentRequestsTableModel();

    // Label input
    QLineEdit* labelInput = receiveCoinsDialog.findChild<QLineEdit*>("reqLabel");
    labelInput->setText("TEST_LABEL_1");

    // Amount input
    BitcoinAmountField* amountInput = receiveCoinsDialog.findChild<BitcoinAmountField*>("reqAmount");
    amountInput->setValue(1);

    // Message input
    QLineEdit* messageInput = receiveCoinsDialog.findChild<QLineEdit*>("reqMessage");
    messageInput->setText("TEST_MESSAGE_1");
    int initialRowCount = requestTableModel->rowCount({});
    QPushButton* requestPaymentButton = receiveCoinsDialog.findChild<QPushButton*>("receiveButton");
    requestPaymentButton->click();
    QString address;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("ReceiveRequestDialog")) {
            ReceiveRequestDialog* receiveRequestDialog = qobject_cast<ReceiveRequestDialog*>(widget);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("payment_header")->text(), QString("Payment information"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("uri_tag")->text(), QString("URI:"));
            QString uri = receiveRequestDialog->QObject::findChild<QLabel*>("uri_content")->text();
            QCOMPARE(uri.count("resatoshi:"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("address_tag")->text(), QString("Address:"));
            QVERIFY(address.isEmpty());
            address = receiveRequestDialog->QObject::findChild<QLabel*>("address_content")->text();
            QVERIFY(!address.isEmpty());

            QCOMPARE(uri.count("amount=0.00000001"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_tag")->text(), QString("Amount:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_content")->text(), QString::fromStdString("0.00000001 " + CURRENCY_UNIT));

            QCOMPARE(uri.count("label=TEST_LABEL_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_tag")->text(), QString("Label:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_content")->text(), QString("TEST_LABEL_1"));

            QCOMPARE(uri.count("message=TEST_MESSAGE_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_tag")->text(), QString("Message:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_content")->text(), QString("TEST_MESSAGE_1"));
        }
    }

    // Clear button
    QPushButton* clearButton = receiveCoinsDialog.findChild<QPushButton*>("clearButton");
    clearButton->click();
    QCOMPARE(labelInput->text(), QString(""));
    QCOMPARE(amountInput->value(), CAmount(0));
    QCOMPARE(messageInput->text(), QString(""));

    // Check addition to history
    int currentRowCount = requestTableModel->rowCount({});
    QCOMPARE(currentRowCount, initialRowCount+1);

    // Check addition to wallet
    std::vector<std::string> requests = walletModel.wallet().getAddressReceiveRequests();
    QCOMPARE(requests.size(), size_t{1});
    RecentRequestEntry entry;
    SpanReader{MakeByteSpan(requests[0])} >> entry;
    QCOMPARE(entry.nVersion, int{1});
    QCOMPARE(entry.id, int64_t{1});
    QVERIFY(entry.date.isValid());
    QCOMPARE(entry.recipient.address, address);
    QCOMPARE(entry.recipient.label, QString{"TEST_LABEL_1"});
    QCOMPARE(entry.recipient.amount, CAmount{1});
    QCOMPARE(entry.recipient.message, QString{"TEST_MESSAGE_1"});
    QCOMPARE(entry.recipient.sPaymentRequest, std::string{});
    QCOMPARE(entry.recipient.authenticatedMerchant, QString{});

    // Check Remove button
    QTableView* table = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
    table->selectRow(currentRowCount-1);
    QPushButton* removeRequestButton = receiveCoinsDialog.findChild<QPushButton*>("removeRequestButton");
    removeRequestButton->click();
    QCOMPARE(requestTableModel->rowCount({}), currentRowCount-1);

    // Check removal from wallet
    QCOMPARE(walletModel.wallet().getAddressReceiveRequests().size(), size_t{0});
}

void TestGUIWatchOnly(interfaces::Node& node, TestChain100Setup& test)
{
    const std::shared_ptr<CWallet>& wallet = SetupDescriptorsWallet(node, test, /*watch_only=*/true);

    // Create widgets and init models
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    MiniGUI mini_gui(node, platformStyle.get());
    mini_gui.initModelForWallet(node, wallet, platformStyle.get());
    WalletModel& walletModel = *mini_gui.walletModel;
    SendCoinsDialog& sendCoinsDialog = mini_gui.sendCoinsDialog;

    // Update walletModel cached balance which will trigger an update for the 'labelBalance' QLabel.
    walletModel.pollBalanceChanged();
    // Check balance in send dialog
    CompareBalance(walletModel, walletModel.wallet().getBalances().balance,
                   sendCoinsDialog.findChild<QLabel*>("labelBalance"));

    // Set change address
    sendCoinsDialog.getCoinControl()->destChange = PKHash{test.coinbaseKey.GetPubKey()};

    // Time to reject "save" PSBT dialog ('SendCoins' locks the main thread until the dialog receives the event).
    QTimer timer;
    timer.setInterval(500);
    QObject::connect(&timer, &QTimer::timeout, [&](){
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("QMessageBox") && widget->objectName().compare("psbt_copied_message") == 0) {
                QMessageBox* dialog = qobject_cast<QMessageBox*>(widget);
                QAbstractButton* button = dialog->button(QMessageBox::Discard);
                button->setEnabled(true);
                button->click();
                timer.stop();
                break;
            }
        }
    });
    timer.start(500);

    // Send tx and verify PSBT copied to the clipboard.
    SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 5 * COIN, /*rbf=*/false, QMessageBox::Save);
    const std::string& psbt_string = QApplication::clipboard()->text().toStdString();
    QVERIFY(!psbt_string.empty());

    // Decode psbt
    std::optional<std::vector<unsigned char>> decoded_psbt = DecodeBase64(psbt_string);
    QVERIFY(decoded_psbt);
    PartiallySignedTransaction psbt;
    std::string err;
    QVERIFY(DecodeRawPSBT(psbt, MakeByteSpan(*decoded_psbt), err));
}

void TestGUI(interfaces::Node& node)
{
    // Set up wallet and chain with 105 blocks (5 mature blocks for spending).
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);

    // "Full" GUI tests, use descriptor wallet
    const std::shared_ptr<CWallet>& desc_wallet = SetupDescriptorsWallet(node, test);
    TestGUI(node, desc_wallet);

    // Legacy watch-only wallet test
    // Verify PSBT creation.
    TestGUIWatchOnly(node, test);
}

} // namespace

void WalletTests::walletTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        qWarning() << "Skipping WalletTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
                      "with 'QT_QPA_PLATFORM=cocoa test_bitcoin-qt' on mac, or else use a linux or windows build.";
        return;
    }
#endif
    TestGUI(m_node);
}

void WalletTests::cpuMinerTests()
{
    TestChain100Setup test;
    test.m_node.mining = interfaces::MakeMining(test.m_node, /*wait_loaded=*/false);
    m_node.setContext(&test.m_node);
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    const std::string address{EncodeDestination(PKHash{test.coinbaseKey.GetPubKey()})};
    CpuMiner miner{m_node};
    QVERIFY(!miner.start({}));
    QVERIFY(miner.start({"invalid-test-address"}));
    QTRY_VERIFY_WITH_TIMEOUT(!miner.running(), 10'000);
    miner.stop();
    QVERIFY(miner.error().find("Invalid address") != std::string::npos);
    QVERIFY(!miner.start({address, address}));
    for (int threads : {1, 2, 4, 8}) {
        const int before{m_node.getNumBlocks()};
        std::vector<std::string> destinations;
        for (int i = 0; i < threads; ++i) {
            CKey key;
            key.MakeNewKey(true);
            destinations.push_back(EncodeDestination(PKHash{key.GetPubKey()}));
        }
        QVERIFY(miner.start(destinations));
        QTRY_VERIFY_WITH_TIMEOUT(m_node.getNumBlocks() > before || !miner.error().empty(), 10'000);
        miner.setPaused(true);
        QTRY_VERIFY_WITH_TIMEOUT(miner.paused(), 10'000);
        QTest::qWait(100);
        const auto paused_hashes = miner.hashes();
        QTest::qWait(100);
        QCOMPARE(miner.hashes(), paused_hashes);
        miner.setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(miner.hashes() > paused_hashes, 10'000);
        miner.requestStop();
        QTRY_VERIFY_WITH_TIMEOUT(!miner.stopping(), 10'000);
        miner.stop();
        QVERIFY(!miner.running());
        QVERIFY2(miner.error().empty(), miner.error().c_str());
        QVERIFY(m_node.getNumBlocks() > before);
        const int stopped{m_node.getNumBlocks()};
        QTest::qWait(50);
        QCOMPARE(m_node.getNumBlocks(), stopped);
    }
}


void WalletTests::cpuMinerMainnetTests()
{
    // Real mainnet templates and difficulty, entirely in-memory, with fake peers.
    // Nothing can be broadcast to an external node.
    TestingSetup test{ChainType::MAIN};
    m_node.setContext(&test.m_node);
    auto& connman = static_cast<ConnmanTestMsg&>(*test.m_node.connman);
    const auto add_peer = [&](bool connected = true) {
        auto* peer = new CNode{0, nullptr, CAddress{}, 0, 0, CService{}, "test-peer",
                              ConnectionType::OUTBOUND_FULL_RELAY, false, 0};
        peer->SetCommonVersion(PROTOCOL_VERSION);
        test.m_node.peerman->InitializeNode(*peer, ServiceFlags(NODE_NETWORK | NODE_WITNESS));
        peer->fSuccessfullyConnected = connected;
        connman.AddTestNode(*peer);
        return peer;
    };
    const auto clear_peers = [&] {
        for (auto* peer : connman.TestNodes()) test.m_node.peerman->FinalizeNode(*peer);
        connman.ClearTestNodes();
    };
    for (int threads : {1, 2, 4, 8}) {
        CpuMiner miner{m_node};
        std::vector<std::string> destinations;
        for (int i = 0; i < threads; ++i) {
            CKey key;
            key.MakeNewKey(true);
            destinations.push_back(EncodeDestination(PKHash{key.GetPubKey()}));
        }
        QVERIFY(miner.start(destinations));
        QTRY_VERIFY_WITH_TIMEOUT(miner.paused(), 5000);
        QCOMPARE(miner.hashes(), uint64_t{0});
        auto* pending = add_peer(false);
        QTest::qWait(100);
        QCOMPARE(miner.hashes(), uint64_t{0});
        pending->fSuccessfullyConnected = true;
        QTRY_VERIFY_WITH_TIMEOUT(miner.hashes() > 0 || !miner.error().empty(), 5000);
        QVERIFY2(miner.error().empty(), miner.error().c_str());
        const auto before = miner.hashes();
        QElapsedTimer work_timer;
        work_timer.start();
        QTest::qWait(2100); // Cross template refresh boundaries at real difficulty.
        qInfo("CPU mining: threads=%d rate=%.0f H/s", threads,
              (miner.hashes() - before) * 1000.0 / work_timer.elapsed());
        m_node.setNetworkActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(miner.paused(), 5000);
        QTest::qWait(100);
        const auto paused_hashes = miner.hashes();
        QTest::qWait(100);
        QCOMPARE(miner.hashes(), paused_hashes);
        m_node.setNetworkActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(miner.hashes() > paused_hashes, 5000);
        clear_peers();
        QTRY_VERIFY_WITH_TIMEOUT(miner.paused(), 5000);
        QTest::qWait(100);
        const auto disconnected_hashes = miner.hashes();
        QTest::qWait(100);
        QCOMPARE(miner.hashes(), disconnected_hashes);
        add_peer();
        QTRY_VERIFY_WITH_TIMEOUT(miner.hashes() > disconnected_hashes, 5000);
        QElapsedTimer stop_timer;
        stop_timer.start();
        miner.requestStop();
        QVERIFY(stop_timer.elapsed() < 100);
        QTRY_VERIFY_WITH_TIMEOUT(!miner.stopping(), 5000);
        miner.stop();
        qInfo("CPU mining: threads=%d stop=%lld ms", threads, static_cast<long long>(stop_timer.elapsed()));
        QVERIFY2(miner.error().empty(), miner.error().c_str());
        clear_peers();
    }
}


void WalletTests::cpuMinerHashTests()
{
    BasicTestingSetup test{ChainType::REGTEST};
    for (int i = 0; i < 100; ++i) {
        CBlockHeader header;
        header.nVersion = test.m_rng.rand32();
        header.hashPrevBlock = test.m_rng.rand256();
        header.hashMerkleRoot = test.m_rng.rand256();
        header.nTime = test.m_rng.rand32();
        header.nBits = test.m_rng.rand32();
        CpuMinerHasher hasher{header};
        for (uint32_t nonce : {uint32_t{0}, uint32_t{1}, uint32_t{0xfffffffe}, uint32_t{0xffffffff}, test.m_rng.rand32()}) {
            header.nNonce = nonce;
            QVERIFY(hasher.hash(nonce) == header.GetHash());
        }
    }

    CBlockHeader header;
    header.hashMerkleRoot = test.m_rng.rand256();
    CpuMinerHasher hasher{header};
    uint64_t reference_sum{0}, optimized_sum{0};
    QElapsedTimer timer;
    timer.start();
    for (uint32_t nonce = 0; nonce < 1'000'000; ++nonce) {
        header.nNonce = nonce;
        reference_sum ^= header.GetHash().GetUint64(0);
    }
    const auto reference_ns = timer.nsecsElapsed();
    timer.restart();
    for (uint32_t nonce = 0; nonce < 1'000'000; ++nonce) {
        optimized_sum ^= hasher.hash(nonce).GetUint64(0);
    }
    const auto optimized_ns = timer.nsecsElapsed();
    QCOMPARE(reference_sum, optimized_sum);
    qInfo("Header hashing: reference=%.0f H/s midstate=%.0f H/s", 1e15 / reference_ns, 1e15 / optimized_ns);
}


void WalletTests::bootstrapPolicyTests()
{
    using namespace std::chrono_literals;
    using Peer = BootstrapPolicy::Peer;
    BootstrapPolicy policy;
    const auto start = BootstrapPolicy::Clock::time_point{};
    QVERIFY(policy.update(start, {}, true).reconnect);
    QVERIFY(!policy.update(start + 59s, {}, true).reconnect);
    QVERIFY(policy.update(start + 60s, {}, true).reconnect);
    const std::vector<Peer> three{{1, true, true}, {2, false, true}, {3, false, true}};
    QVERIFY(policy.update(start + 61s, three, true).disconnect.empty());
    QVERIFY(policy.update(start + 200s, three, true).disconnect.empty()); // Bootstrap is not one of the three replacements.
    auto four = three;
    four.push_back({4, false, true});
    QVERIFY(policy.update(start + 201s, four, true).disconnect.empty());
    QVERIFY(policy.update(start + 260s, four, true).disconnect.empty());
    auto action = policy.update(start + 261s, four, true);
    QVERIFY(action.disconnect == std::vector<int64_t>{1});
    QVERIFY(!action.reconnect);
    four[3].id = 5; // A replacement connection must earn its own stable interval.
    QVERIFY(policy.update(start + 262s, four, true).disconnect.empty());
    QVERIFY(policy.update(start + 321s, four, true).disconnect.empty());
    QVERIFY(policy.update(start + 322s, four, true).disconnect == std::vector<int64_t>{1});
    four[3].ready = false;
    QVERIFY(policy.update(start + 323s, four, true).disconnect.empty());
    four[3].ready = true;
    QVERIFY(policy.update(start + 324s, four, true).disconnect.empty());
    QVERIFY(policy.update(start + 383s, four, true).disconnect.empty());
    QVERIFY(policy.update(start + 384s, four, true).disconnect == std::vector<int64_t>{1});
    four.push_back({6, true, true}); // Both bootstrap endpoints can be retired.
    QVERIFY(policy.update(start + 385s, four, true).disconnect == (std::vector<int64_t>{1, 6}));
    QVERIFY(!policy.update(start + 386s, {{2, false, true}}, true).reconnect);
    QVERIFY(policy.update(start + 387s, {{7, false, false}}, true).reconnect); // Inbound-only connections cannot suppress recovery.
    QVERIFY(!policy.update(start + 388s, {}, false).reconnect);
    QVERIFY(policy.update(start + 389s, {}, true).reconnect);
}


void WalletTests::bootstrapManagerTests()
{
    using namespace std::chrono_literals;
    TestingSetup test{ChainType::MAIN};
    m_node.setContext(&test.m_node);
    auto& connman = static_cast<ConnmanTestMsg&>(*test.m_node.connman);
    const auto add_peer = [&](int64_t id, const char* ip, const std::string& name,
                              ConnectionType type = ConnectionType::OUTBOUND_FULL_RELAY) {
        auto address = LookupNumeric(ip, 19333);
        auto* peer = new CNode{id, nullptr, CAddress{address, NODE_NETWORK}, 0, 0, CService{}, name,
                              type, false, 0};
        {
            LOCK(NetEventsInterface::g_msgproc_mutex);
            connman.Handshake(*peer, true, ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                              ServiceFlags(NODE_NETWORK | NODE_WITNESS), PROTOCOL_VERSION, true);
        }
        connman.AddTestNode(*peer);
        return peer;
    };
    const auto clear_peers = [&] {
        for (auto* peer : connman.TestNodes()) test.m_node.peerman->FinalizeNode(*peer);
        connman.ClearTestNodes();
    };
    int connects{0}, removes{0};
    BootstrapManager manager{m_node,
        [&](const std::string&, bool add) { if (add) ++connects; else ++removes; return true; }};
    const auto start = BootstrapManager::Clock::time_point{};
    manager.poll(start);
    QCOMPARE(connects, 2);
    manager.poll(start + 1s);
    auto* bootstrap_ip = add_peer(1, "192.0.2.10", "192.0.2.10:19333");
    auto* bootstrap_name = add_peer(5, "192.0.2.11", "resatoshi-seed.duckdns.org:19333");
    auto* regular1 = add_peer(2, "8.8.8.8", "8.8.8.8:19333");
    auto* regular2 = add_peer(3, "8.8.4.4", "8.8.4.4:19333");
    auto* regular3 = add_peer(4, "1.1.1.1", "1.1.1.1:19333");
    // Exercise Core's cached resolution using a fake resolver and existing
    // connections. No DNS or socket connection can reach the outside network.
    const auto original_lookup = g_dns_lookup;
    const auto first_address = LookupNumeric("192.0.2.10", 19333);
    const auto second_address = LookupNumeric("192.0.2.11", 19333);
    g_dns_lookup = [&](const std::string& host, bool) {
        if (host.starts_with("resatoshi-seed.freeddns.org")) return std::vector<CNetAddr>{first_address};
        if (host.starts_with("resatoshi-seed.duckdns.org")) return std::vector<CNetAddr>{second_address};
        return original_lookup(host, false);
    };
    for (const auto& seed : Params().DNSSeeds()) {
        const auto destination = seed + ":19333";
        connman.OpenNetworkConnection(CAddress{}, false, {}, destination.c_str(), ConnectionType::MANUAL, false);
    }
    g_dns_lookup = original_lookup;
    std::set<CNetAddr> cached;
    QVERIFY(m_node.getSeedAddresses(cached));
    QCOMPARE(cached.size(), size_t{2});
    // Even three stable peers must not retire bootstrap during IBD.
    QVERIFY(m_node.isInitialBlockDownload());
    manager.poll(start + 2s);
    manager.poll(start + 100s);
    QVERIFY(!bootstrap_ip->fDisconnect);
    QCOMPARE(removes, 0);
    SetMockTime(Params().GenesisBlock().GetBlockTime() + 1);
    {
        LOCK(cs_main);
        test.m_node.chainman->UpdateIBDStatus();
    }
    QVERIFY(!m_node.isInitialBlockDownload());
    SetMockTime(0);
    add_peer(7, "9.9.9.9", "inbound-a", ConnectionType::INBOUND);
    add_peer(8, "4.2.2.2", "inbound-b", ConnectionType::INBOUND);
    add_peer(9, "208.67.222.222", "inbound-c", ConnectionType::INBOUND);
    manager.poll(start + 101s);
    manager.poll(start + 161s);
    // Three inbound peers and duplicate outbound network groups cannot retire
    // bootstrap. All addresses are synthetic test connections, without sockets.
    QVERIFY(!bootstrap_ip->fDisconnect);
    QCOMPARE(removes, 0);
    regular2->fDisconnect = true;
    regular2 = add_peer(10, "9.9.9.10", "independent-outbound");
    manager.poll(start + 162s);
    // A failed try-lock observation must not erase the 60-second interval.
    std::latch locked{1}, unlock{1};
    std::thread busy{[&] {
        LOCK(cs_main);
        locked.count_down();
        unlock.wait();
    }};
    locked.wait();
    manager.poll(start + 191s);
    unlock.count_down();
    busy.join();
    manager.poll(start + 221s);
    QVERIFY(!bootstrap_ip->fDisconnect);
    QVERIFY(!bootstrap_name->fDisconnect);
    manager.poll(start + 222s);
    QVERIFY(bootstrap_ip->fDisconnect);
    QVERIFY(bootstrap_name->fDisconnect);
    QCOMPARE(removes, 2);
    QVERIFY(!regular1->fDisconnect && !regular2->fDisconnect && !regular3->fDisconnect);
    clear_peers();
    add_peer(6, "4.2.2.2", "4.2.2.2:19333");
    manager.poll(start + 223s);
    QCOMPARE(connects, 2);
    clear_peers();
    add_peer(11, "8.8.8.8", "inbound-remainder", ConnectionType::INBOUND);
    manager.poll(start + 224s);
    QTRY_COMPARE_WITH_TIMEOUT(connects, 4, 5000);
    m_node.setNetworkActive(false);
    manager.poll(start + 285s);
    QCOMPARE(connects, 4);
    m_node.setNetworkActive(true);
    manager.poll(start + 286s);
    QTRY_COMPARE_WITH_TIMEOUT(connects, 6, 5000);
    // Core registration is nonblocking and manager destruction preserves
    // an entry explicitly supplied by the user.
    const std::string user_seed{"resatoshi-seed.freeddns.org:19333"};
    QVERIFY(connman.AddNode({user_seed, false}));
    {
        BootstrapManager core_manager{m_node};
        core_manager.poll(start);
        QCOMPARE(connman.GetAddedNodeInfo(/*include_connected=*/true).size(), size_t{2});
    }
    const auto added = connman.GetAddedNodeInfo(/*include_connected=*/true);
    QCOMPARE(added.size(), size_t{1});
    QCOMPARE(added[0].m_params.m_added_node, user_seed);
    QVERIFY(connman.RemoveAddedNode(user_seed));
}

void WalletTests::cpuMinerOldTipTests()
{
    CBlock block;
    // Public mainnet block 1; no operational node or wallet is used.
    QVERIFY(DecodeHexBlk(block, "00000020ec740156fa8f5bbe37f8ad3b43144f68746f9c960046ddfc45328179030000003955d305c79fb00d64765a20de3070475f4f293e3465b670abd8db7075e9caa336fd9f6a12a1031d0f59000001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025100feffffff0200f2052a01000000160014274db26c53b6d883fa72cda65452e186fa1cd43d0000000000000000266a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf90120000000000000000000000000000000000000000000000000000000000000000000000000"));
    for (bool body_first : {false, true}) {
        TestingSetup test{ChainType::MAIN};
        m_node.setContext(&test.m_node);
        SetMockTime(block.GetBlockTime() + 5 * 24 * 60 * 60);
        const auto accept_body = [&] {
            return test.m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block),
                /*force_processing=*/true, /*min_pow_checked=*/true, nullptr);
        };
        if (body_first) QVERIFY(accept_body());
        auto& connman = static_cast<ConnmanTestMsg&>(*test.m_node.connman);
        auto* peer = new CNode{0, nullptr, CAddress{}, 0, 0, CService{}, "old-tip-test",
                              ConnectionType::OUTBOUND_FULL_RELAY, false, 0};
        {
            LOCK(NetEventsInterface::g_msgproc_mutex);
            connman.Handshake(*peer, true, ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                              ServiceFlags(NODE_NETWORK | NODE_WITNESS), PROTOCOL_VERSION, true);
        }
        connman.AddTestNode(*peer);
        const auto announce_header = [&] {
            LOCK(NetEventsInterface::g_msgproc_mutex);
            std::vector<CBlock> headers{CBlock{static_cast<const CBlockHeader&>(block)}};
            connman.FlushSendBuffer(*peer);
            connman.ReceiveMsgFrom(*peer, NetMsg::Make(NetMsgType::HEADERS, TX_WITH_WITNESS(headers)));
            peer->fPauseSend = false;
            connman.ProcessMessagesOnce(*peer);
        };
        if (!body_first) announce_header();
        // Wait either for the missing block body or for peer confirmation.
        QVERIFY(!m_node.isReadyToMine());
        CKey key;
        key.MakeNewKey(true);
        CpuMiner miner{m_node};
        QVERIFY(miner.start({EncodeDestination(PKHash{key.GetPubKey()})}));
        QTRY_VERIFY_WITH_TIMEOUT(miner.paused(), 5000);
        QTest::qWait(100);
        QCOMPARE(miner.hashes(), uint64_t{0});
        miner.setPaused(true);
        if (body_first) announce_header();
        else QVERIFY(accept_body());
        QCOMPARE(m_node.getNumBlocks(), 1);
        // The tip is still five days old and Core still reports IBD. Only the
        // local mining decision is relaxed, after actual P2P header exchange.
        QVERIFY(m_node.isInitialBlockDownload());
        QVERIFY(m_node.isReadyToMine());
        auto* attacker = new CNode{1, nullptr, CAddress{}, 0, 0, CService{}, "false-height",
                                  ConnectionType::INBOUND, false, 0};
        {
            LOCK(NetEventsInterface::g_msgproc_mutex);
            connman.Handshake(*attacker, true, NODE_NONE,
                              ServiceFlags(NODE_NETWORK | NODE_WITNESS), PROTOCOL_VERSION, true,
                              std::numeric_limits<int32_t>::max());
        }
        connman.AddTestNode(*attacker);
        CNodeStateStats attacker_stats;
        QVERIFY(test.m_node.peerman->GetNodeStateStats(attacker->GetId(), attacker_stats));
        QCOMPARE(attacker_stats.m_starting_height, std::numeric_limits<int32_t>::max());
        QVERIFY(m_node.isConnected(attacker->GetId()));
        QVERIFY(m_node.isReadyToMine());
        miner.setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(miner.hashes() > 0 || !miner.error().empty(), 5000);
        m_node.setNetworkActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(miner.paused(), 5000);
        QVERIFY(!m_node.isReadyToMine());
        const auto before = miner.hashes();
        m_node.setNetworkActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(miner.hashes() > before || !miner.error().empty(), 5000);
        miner.stop();
        QVERIFY2(miner.error().empty(), miner.error().c_str());
        SetMockTime(0);
        test.m_node.peerman->FinalizeNode(*attacker);
        test.m_node.peerman->FinalizeNode(*peer);
        connman.ClearTestNodes();
    }
}
