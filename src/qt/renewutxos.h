// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_RENEWUTXOS_H
#define BITCOIN_QT_RENEWUTXOS_H

#include <consensus/amount.h>
#include <primitives/transaction.h>

#include <QWidget>

#include <vector>

class QLabel;
class QPushButton;
class QTableWidget;
class WalletModel;

class RenewUtxos : public QWidget
{
    Q_OBJECT
public:
    explicit RenewUtxos(WalletModel* wallet_model, QWidget* parent = nullptr);
    void setBlockHeight(int height);

Q_SIGNALS:
    void coinsSent();

private Q_SLOTS:
    void refresh();
    void selectNearExpiry();
    void selectAllEligible();
    void selectionChanged();
    void renew();

private:
    struct CoinRow {
        COutPoint outpoint;
        CAmount amount;
        int expiry_height;
        int input_bytes;
        bool eligible;
    };
    void clearPreview();
    std::vector<CoinRow> selectedCoins() const;
    CAmount estimatedFee(const std::vector<CoinRow>& coins, bool* used_fallback = nullptr) const;
    QString formatAmount(CAmount amount) const;

    WalletModel* const m_wallet_model;
    QTableWidget* m_table;
    QLabel* m_summary;
    QLabel* m_destination;
    QLabel* m_status;
    QPushButton* m_renew;
    std::vector<CoinRow> m_rows;
    std::vector<QString> m_preview_destinations;
    std::vector<Txid> m_sent_txids;
    int m_height{0};
    bool m_preview_ready{false};
    bool m_refreshing{false};
};

#endif // BITCOIN_QT_RENEWUTXOS_H
