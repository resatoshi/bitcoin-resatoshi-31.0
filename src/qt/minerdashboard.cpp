// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/minerdashboard.h>

#include <qt/clientmodel.h>
#include <qt/bootstrapmanager.h>
#include <qt/cpuminer.h>
#include <qt/guiutil.h>
#include <qt/overviewpage.h>
#include <qt/renewutxos.h>
#include <qt/sendcoinsdialog.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <outputtype.h>


#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QTableWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QLabel* ValueLabel(const QString& text)
{
    auto* label = new QLabel(text);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
}

MinerDashboard::MinerDashboard(WalletModel* wallet_model, const PlatformStyle* platform_style, QWidget* parent)
    : QWidget(parent), m_wallet_model(wallet_model)
{
    setObjectName("minerDashboard");
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto* controls = new QVBoxLayout;
    auto* node_box = new QGroupBox(tr("Node & CPU Mining"));
    auto* node_layout = new QVBoxLayout(node_box);
    m_sync = ValueLabel(tr("Starting node…"));
    m_sync->setObjectName("syncStatus");
    m_peers = ValueLabel(tr("Connected Peers: 0"));
    m_peers->setObjectName("connectedPeers");
    m_indicator = ValueLabel(tr("● Mining stopped"));
    m_indicator->setStyleSheet("color:#777; font-weight:600;");
    m_hashrate = ValueLabel(tr("Hashrate: 0 H/s"));
    m_hashrate->setObjectName("hashrate");
    m_threads = new QComboBox;
    m_threads->setObjectName("miningThreads");
    for (int count : {1, 2, 4, 8}) m_threads->addItem(tr("%1 CPU thread(s)").arg(count), count);
    m_mining_button = new QPushButton(tr("Start Mining"));
    m_mining_button->setObjectName("miningToggle");
    m_mining_button->setEnabled(false);
    node_layout->addWidget(m_sync);
    node_layout->addWidget(m_peers);
    node_layout->addWidget(m_indicator);
    node_layout->addWidget(m_hashrate);
    node_layout->addWidget(m_threads);
    node_layout->addWidget(m_mining_button);

    auto* wallet_box = new QGroupBox(tr("Wallet & Safety"));
    auto* wallet_layout = new QVBoxLayout(wallet_box);
    wallet_layout->addWidget(new QLabel(tr("Mining rewards are paid only to this local wallet.")));
    m_address = ValueLabel(tr("Creating a receiving address…"));
    m_address->setObjectName("rewardAddress");
    m_address->setWordWrap(true);
    wallet_layout->addWidget(m_address);
    auto* copy = new QPushButton(tr("Copy Receiving Address"));
    wallet_layout->addWidget(copy);
    m_security = ValueLabel({});
    m_security->setWordWrap(true);
    wallet_layout->addWidget(m_security);
    auto* encrypt = new QPushButton(tr("Encrypt Wallet"));
    auto* backup = new QPushButton(tr("Back Up Wallet"));
    auto* restore = new QPushButton(tr("Restore Backup"));
    wallet_layout->addWidget(encrypt);
    wallet_layout->addWidget(backup);
    wallet_layout->addWidget(restore);
    wallet_layout->addWidget(new QLabel(tr("Keep the passphrase and backup offline. Nobody can recover a lost passphrase.")));

    auto* recovery_box = new QGroupBox(tr("Emergency peer recovery"));
    recovery_box->setObjectName("recoveryPanel");
    auto* recovery_layout = new QVBoxLayout(recovery_box);
    auto* recovery_help = new QLabel(tr("Enter an IP or DDNS shared by a synchronized ReSatoshi peer. The peer must accept TCP 19333."));
    recovery_help->setWordWrap(true);
    recovery_layout->addWidget(recovery_help);
    m_recovery_input = new QLineEdit;
    m_recovery_input->setObjectName("recoveryAddressInput");
    m_recovery_input->setPlaceholderText(tr("IP:19333 or DDNS:19333"));
    m_recovery_input->setMaxLength(300);
    recovery_layout->addWidget(m_recovery_input);
    m_recovery_save = new QPushButton(tr("Save address"));
    m_recovery_save->setObjectName("recoverySave");
    recovery_layout->addWidget(m_recovery_save);
    m_recovery_table = new QTableWidget(0, 2);
    m_recovery_table->setObjectName("recoveryTable");
    m_recovery_table->setHorizontalHeaderLabels({tr("Address"), tr("Status")});
    m_recovery_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_recovery_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recovery_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recovery_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recovery_table->setMaximumHeight(150);
    recovery_layout->addWidget(m_recovery_table);
    auto* recovery_buttons = new QHBoxLayout;
    m_recovery_connect = new QPushButton(tr("Connect once"));
    m_recovery_connect->setObjectName("recoveryConnect");
    m_recovery_remove = new QPushButton(tr("Remove"));
    m_recovery_remove->setObjectName("recoveryRemove");
    recovery_buttons->addWidget(m_recovery_connect);
    recovery_buttons->addWidget(m_recovery_remove);
    recovery_layout->addLayout(recovery_buttons);
    auto* recovery_policy = new QLabel(tr("Saved addresses retry after 60 seconds with no connected peers. Temporary bootstrap connections close after 3 ordinary peers stay stable for 2 minutes."));
    recovery_policy->setWordWrap(true);
    recovery_layout->addWidget(recovery_policy);
    const auto selected_address = [this] {
        const int row = m_recovery_table->currentRow();
        return row < 0 ? std::string{} : m_recovery_table->item(row, 0)->text().toStdString();
    };
    connect(m_recovery_save, &QPushButton::clicked, this, [this] {
        if (!m_client_model || !m_client_model->bootstrapManager()) return;
        if (!m_client_model->bootstrapManager()->saveAddress(m_recovery_input->text().toStdString())) {
            QMessageBox::warning(this, tr("Recovery address"), tr("Enter a valid IP or DDNS address and port. Duplicate addresses are not added; at most 32 addresses can be saved. Also check that settings storage is writable."));
            return;
        }
        m_recovery_input->clear();
        refreshRecovery();
        m_recovery_table->selectRow(m_recovery_table->rowCount() - 1);
    });
    connect(m_recovery_connect, &QPushButton::clicked, this, [this, selected_address] {
        if (m_client_model && m_client_model->bootstrapManager()) {
            if (!m_client_model->bootstrapManager()->connectAddress(selected_address())) QMessageBox::warning(this, tr("Recovery connection"), tr("Connection could not be queued. Check that networking is enabled and no connection-only configuration is active."));
            refreshRecovery();
        }
    });
    connect(m_recovery_remove, &QPushButton::clicked, this, [this, selected_address] {
        if (m_client_model && m_client_model->bootstrapManager()) {
            if (!m_client_model->bootstrapManager()->removeAddress(selected_address())) QMessageBox::warning(this, tr("Recovery address"), tr("Could not save the recovery list. Check that settings storage is writable."));
            refreshRecovery();
        }
    });
    connect(m_recovery_table, &QTableWidget::itemSelectionChanged, this, &MinerDashboard::refreshRecovery);
    controls->addWidget(node_box);
    controls->addWidget(recovery_box);
    controls->addWidget(wallet_box);
    controls->addStretch();
    auto* left = new QWidget;
    left->setLayout(controls);
    left->setMinimumWidth(275);

    m_overview = new OverviewPage(platform_style);
    m_overview->setWalletModel(wallet_model);
    m_renew = new RenewUtxos(wallet_model);
    auto* center_layout = new QVBoxLayout;
    center_layout->setContentsMargins(0, 0, 0, 0);
    center_layout->addWidget(m_overview, 1);
    auto* renew_box = new QGroupBox(tr("Renew UTXOs"));
    auto* renew_layout = new QVBoxLayout(renew_box);
    renew_layout->addWidget(m_renew);
    center_layout->addWidget(renew_box, 2);
    auto* center = new QWidget;
    center->setLayout(center_layout);

    m_send = new SendCoinsDialog(platform_style);
    m_send->setObjectName("sendPanel");
    m_send->setWindowFlags(Qt::Widget);
    m_send->setModel(wallet_model);
    auto* send_scroll = new QScrollArea;
    send_scroll->setWidgetResizable(true);
    send_scroll->setWidget(m_send);
    send_scroll->setMinimumWidth(390);

    auto* left_scroll = new QScrollArea;
    left_scroll->setWidgetResizable(true);
    left_scroll->setWidget(left);
    left_scroll->setMinimumWidth(310);
    root->addWidget(left_scroll, 0);
    root->addWidget(center, 1);
    root->addWidget(send_scroll, 1);

    connect(m_mining_button, &QPushButton::clicked, this, &MinerDashboard::toggleMining);
    connect(copy, &QPushButton::clicked, this, &MinerDashboard::copyAddress);
    connect(encrypt, &QPushButton::clicked, this, &MinerDashboard::encryptRequested);
    connect(backup, &QPushButton::clicked, this, &MinerDashboard::backupRequested);
    connect(restore, &QPushButton::clicked, this, &MinerDashboard::restoreRequested);
    connect(m_overview, &OverviewPage::transactionClicked, this, &MinerDashboard::transactionClicked);
    connect(m_overview, &OverviewPage::outOfSyncWarningClicked, this, &MinerDashboard::outOfSyncWarningClicked);
    connect(m_send, &SendCoinsDialog::coinsSent, this, [this] {
        QTimer::singleShot(0, m_send, &QWidget::show);
        Q_EMIT coinsSent();
    });
    connect(m_renew, &RenewUtxos::coinsSent, this, &MinerDashboard::coinsSent);

    connect(wallet_model, &WalletModel::encryptionStatusChanged, this, &MinerDashboard::refreshStatus);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MinerDashboard::refreshStatus);
    timer->start(1000);
    m_hash_clock.start();

    std::vector<std::string> initial;
    ensureMiningAddresses(1, initial);
    refreshStatus();
}

MinerDashboard::~MinerDashboard()
{
    if (m_miner) m_miner->stop();
}

void MinerDashboard::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    m_overview->setClientModel(client_model);
    m_send->setClientModel(client_model);
    if (client_model) {
        m_miner = std::make_unique<CpuMiner>(client_model->node());
        m_last_hashes = 0;
        m_hash_clock.restart();
    }
    refreshStatus();
}

void MinerDashboard::setPrivacy(bool privacy) { m_overview->setPrivacy(privacy); }
void MinerDashboard::showOutOfSyncWarning(bool show) { m_overview->showOutOfSyncWarning(show); }

bool MinerDashboard::synchronized() const
{
    return m_client_model && m_client_model->node().isReadyToMine();
}

bool MinerDashboard::ensureMiningAddresses(int count, std::vector<std::string>& addresses)
{
    while (static_cast<int>(m_mining_addresses.size()) < count) {
        auto destination = m_wallet_model->wallet().getNewDestination(OutputType::BECH32, "CPU mining reward");
        if (!destination) {
            QMessageBox::critical(this, tr("Wallet error"), tr("Could not create a mining reward address."));
            return false;
        }
        m_mining_addresses.push_back(EncodeDestination(*destination));
    }
    addresses.assign(m_mining_addresses.begin(), m_mining_addresses.begin() + count);
    m_address->setText(QString::fromStdString(m_mining_addresses.front()));
    return true;
}

void MinerDashboard::toggleMining()
{
    if (!m_miner) return;
    if (m_miner->running()) {
        m_mining_button->setEnabled(false);
        m_miner->requestStop();
        refreshStatus();
        return;
    }
    if (!synchronized()) {
        QMessageBox::information(this, tr("Not synchronized"), tr("Mining can start after the node is fully synchronized."));
        return;
    }
    const int count = m_threads->currentData().toInt();
    std::vector<std::string> addresses;
    if (!ensureMiningAddresses(count, addresses)) return;
    m_last_hashes = 0;
    m_hash_clock.restart();
    m_miner->setPaused(false);
    m_miner->start(std::move(addresses));
    refreshStatus();
}

void MinerDashboard::refreshStatus()
{
    refreshRecovery();
    const int blocks = m_client_model ? m_client_model->getNumBlocks() : 0;
    const int headers = m_client_model ? m_client_model->getHeaderTipHeight() : 0;
    const int peers = m_client_model ? m_client_model->getNumConnections() : 0;
    m_renew->setBlockHeight(blocks);
    m_sync->setText(synchronized() ? tr("Synchronized — Block %1").arg(blocks)
                                   : tr("Synchronizing — Blocks %1 / Headers %2").arg(blocks).arg(headers));
    m_peers->setText(tr("Connected Peers: %1").arg(peers));
    const bool mining = m_miner && m_miner->running();
    const bool stopping = m_miner && m_miner->stopping();
    m_indicator->setText(mining ? tr("● Mining — %1 thread(s)").arg(m_miner->threadCount())
                                : tr("● Mining stopped"));
    if (mining && m_miner->paused()) m_indicator->setText(tr("● Mining paused — waiting for synchronization and peers"));
    if (stopping) m_indicator->setText(tr("● Stopping mining…"));
    m_indicator->setStyleSheet(mining ? "color:#18a558; font-weight:700;" : "color:#777; font-weight:600;");
    const uint64_t hashes = m_miner ? m_miner->hashes() : 0;
    const auto elapsed_ns = m_hash_clock.nsecsElapsed();
    const double rate = elapsed_ns > 0 ? (hashes - m_last_hashes) * 1e9 / elapsed_ns : 0;
    m_hash_clock.restart();
    m_hashrate->setText(tr("Hashrate: %1 H/s").arg(rate, 0, 'f', 0));
    m_last_hashes = hashes;
    m_mining_button->setText(mining ? tr("Stop Mining") : tr("Start Mining"));
    m_mining_button->setEnabled(!stopping && (mining || synchronized()));
    m_threads->setEnabled(!mining);
    if (m_miner && !m_miner->error().empty()) {
        m_hashrate->setText(tr("Mining error: %1").arg(QString::fromStdString(m_miner->error())));
        m_miner->requestStop();
    }
    switch (m_wallet_model->getEncryptionStatus()) {
    case WalletModel::Unencrypted:
        m_security->setText(tr("Warning: wallet is not encrypted. Encrypt it before receiving funds, then make a backup."));
        m_security->setStyleSheet("color:#b45309;");
        break;
    case WalletModel::Locked:
        m_security->setText(tr("Wallet encrypted and locked."));
        m_security->setStyleSheet("color:#15803d;");
        break;
    case WalletModel::Unlocked:
        m_security->setText(tr("Wallet encrypted and temporarily unlocked."));
        m_security->setStyleSheet("color:#15803d;");
        break;
    default:
        m_security->setText(tr("This wallet has no private keys."));
    }
}

void MinerDashboard::copyAddress()
{
    QApplication::clipboard()->setText(m_address->text());
}

void MinerDashboard::refreshRecovery()
{
    auto* manager = m_client_model ? m_client_model->bootstrapManager() : nullptr;
    m_recovery_save->setEnabled(manager != nullptr);
    m_recovery_input->setEnabled(manager != nullptr);
    const auto selected = m_recovery_table->currentRow() >= 0 ? m_recovery_table->item(m_recovery_table->currentRow(), 0)->text() : QString{};
    const QSignalBlocker blocker{m_recovery_table};
    const std::vector<std::string> empty;
    const auto& addresses = manager ? manager->recoveryAddresses() : empty;
    m_recovery_table->setRowCount(static_cast<int>(addresses.size()));
    for (int row = 0; row < static_cast<int>(addresses.size()); ++row) {
        const QString address = QString::fromStdString(addresses[row]);
        if (!m_recovery_table->item(row, 0)) m_recovery_table->setItem(row, 0, new QTableWidgetItem);
        m_recovery_table->item(row, 0)->setText(address);
        QString status;
        switch (manager->recoveryStatus(addresses[row])) {
        case CConnman::OneTryStatus::SAVED: status = tr("Saved"); break;
        case CConnman::OneTryStatus::CONNECTING: status = tr("Connecting"); break;
        case CConnman::OneTryStatus::CONNECTED: status = tr("Connected"); break;
        case CConnman::OneTryStatus::FAILED: status = tr("Failed"); break;
        case CConnman::OneTryStatus::WRONG_NETWORK: status = tr("Wrong network"); break;
        }
        if (!m_recovery_table->item(row, 1)) m_recovery_table->setItem(row, 1, new QTableWidgetItem);
        m_recovery_table->item(row, 1)->setText(status);
        if (address == selected) m_recovery_table->selectRow(row);
    }
    const int row = m_recovery_table->currentRow();
    const bool valid = manager && row >= 0 && row < static_cast<int>(addresses.size());
    m_recovery_remove->setEnabled(valid);
    m_recovery_connect->setEnabled(valid && m_client_model->node().getNetworkActive() &&
        manager->recoveryStatus(addresses[row]) != CConnman::OneTryStatus::CONNECTING &&
        manager->recoveryStatus(addresses[row]) != CConnman::OneTryStatus::CONNECTED);
}
