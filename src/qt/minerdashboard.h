// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_MINERDASHBOARD_H
#define BITCOIN_QT_MINERDASHBOARD_H

#include <QWidget>
#include <QElapsedTimer>

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

class ClientModel;
class CpuMiner;
class QLabel;
class QLineEdit;
class QTableWidget;
class OverviewPage;
class PlatformStyle;
class QPushButton;
class QComboBox;
class RenewUtxos;
class SendCoinsDialog;
class WalletModel;
class QModelIndex;

class MinerDashboard : public QWidget
{
    Q_OBJECT
public:
    MinerDashboard(WalletModel* wallet_model, const PlatformStyle* platform_style, QWidget* parent = nullptr);
    ~MinerDashboard();

    void setClientModel(ClientModel* client_model);
    void setPrivacy(bool privacy);
    void showOutOfSyncWarning(bool show);

Q_SIGNALS:
    void transactionClicked(const QModelIndex& index);
    void outOfSyncWarningClicked();
    void coinsSent();
    void encryptRequested();
    void backupRequested();
    void restoreRequested();

private Q_SLOTS:
    void toggleMining();
    void refreshStatus();
    void copyAddress();
    void refreshRecovery();

private:
    bool synchronized() const;
    bool ensureMiningAddresses(int count, std::vector<std::string>& addresses);

    WalletModel* const m_wallet_model;
    ClientModel* m_client_model{nullptr};
    OverviewPage* m_overview;
    SendCoinsDialog* m_send;
    RenewUtxos* m_renew;
    QComboBox* m_threads;
    QPushButton* m_mining_button;
    QLabel* m_sync;
    QLabel* m_peers;
    QLabel* m_hashrate;
    QLabel* m_indicator;
    QLabel* m_address;
    QLabel* m_security;
    QLineEdit* m_recovery_input;
    QTableWidget* m_recovery_table;
    QPushButton* m_recovery_save;
    QPushButton* m_recovery_connect;
    QPushButton* m_recovery_remove;
    std::unique_ptr<CpuMiner> m_miner;
    std::vector<std::string> m_mining_addresses;
    uint64_t m_last_hashes{0};
    QElapsedTimer m_hash_clock;
};

#endif // BITCOIN_QT_MINERDASHBOARD_H
