// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/minerdashboard.h>

#include <qt/clientmodel.h>
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
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
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

    controls->addWidget(node_box);
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

    root->addWidget(left, 0);
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
