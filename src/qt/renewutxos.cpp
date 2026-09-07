// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/renewutxos.h>

#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsrecipient.h>
#include <qt/walletmodel.h>
#include <qt/walletmodeltransaction.h>

#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/recycle.h>
#include <outputtype.h>
#include <policy/feerate.h>
#include <policy/fees/block_policy_estimator.h>
#include <wallet/coincontrol.h>

#include <algorithm>
#include <exception>
#include <memory>

#include <QAbstractItemView>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int NEAR_EXPIRY_BLOCKS{52'560}; // About one year at ten minutes per block.
constexpr size_t MAX_INPUTS_PER_TRANSACTION{100};
constexpr unsigned int TX_OVERHEAD_VBYTES{11};
constexpr unsigned int P2WPKH_OUTPUT_VBYTES{31};
constexpr unsigned int UNKNOWN_INPUT_VBYTES{148};
}

RenewUtxos::RenewUtxos(WalletModel* wallet_model, QWidget* parent)
    : QWidget(parent), m_wallet_model(wallet_model)
{
    setObjectName("renewUtxosPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    auto* explanation = new QLabel(tr("Renew selected UTXOs by spending them to a fresh address in this wallet. No expiry data is edited."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    m_table = new QTableWidget(0, 6);
    m_table->setObjectName("renewUtxosTable");
    m_table->setHorizontalHeaderLabels({tr("Renew"), tr("Amount"), tr("Created"), tr("Expires"), tr("Remaining"), tr("Outpoint")});
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table, 1);

    auto* choices = new QHBoxLayout;
    auto* near_expiry_button = new QPushButton(tr("Select Near Expiry"));
    near_expiry_button->setObjectName("selectNearExpiry");
    auto* all = new QPushButton(tr("Select All Eligible"));
    all->setObjectName("selectAllRenewable");
    choices->addWidget(near_expiry_button);
    choices->addWidget(all);
    layout->addLayout(choices);

    m_summary = new QLabel(tr("Select one or more eligible UTXOs."));
    m_summary->setObjectName("renewSummary");
    m_summary->setWordWrap(true);
    m_destination = new QLabel(tr("New address: created during fee preview"));
    m_destination->setObjectName("renewDestination");
    m_destination->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_destination->setWordWrap(true);
    m_status = new QLabel;
    m_status->setObjectName("renewStatus");
    m_status->setWordWrap(true);
    m_renew = new QPushButton(tr("Renew UTXOs — Preview"));
    m_renew->setObjectName("renewUtxosButton");
    m_renew->setEnabled(false);
    layout->addWidget(m_summary);
    layout->addWidget(m_destination);
    layout->addWidget(m_status);
    layout->addWidget(m_renew);

    connect(near_expiry_button, &QPushButton::clicked, this, &RenewUtxos::selectNearExpiry);
    connect(all, &QPushButton::clicked, this, &RenewUtxos::selectAllEligible);
    connect(m_table, &QTableWidget::itemChanged, this, &RenewUtxos::selectionChanged);
    connect(m_renew, &QPushButton::clicked, this, &RenewUtxos::renew);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &RenewUtxos::refresh);
    timer->start(5000);
    refresh();
}

void RenewUtxos::setBlockHeight(int height)
{
    if (m_height == height) return;
    m_height = height;
    refresh();
}

QString RenewUtxos::formatAmount(CAmount amount) const
{
    return BitcoinUnits::formatWithUnit(m_wallet_model->getOptionsModel()->getDisplayUnit(), amount);
}

void RenewUtxos::refresh()
{
    if (!m_sent_txids.empty()) {
        QStringList lines;
        for (const Txid& txid : m_sent_txids) {
            interfaces::WalletTxStatus status;
            int blocks{0};
            int64_t block_time{0};
            if (m_wallet_model->wallet().tryGetTxStatus(txid, status, blocks, block_time)) {
                if (status.depth_in_main_chain > 0) {
                    lines.push_back(tr("%1 — confirmed at block %2 (%3 confirmation(s))")
                        .arg(QString::fromStdString(txid.ToString())).arg(status.block_height).arg(status.depth_in_main_chain));
                } else {
                    lines.push_back(tr("%1 — unconfirmed").arg(QString::fromStdString(txid.ToString())));
                }
            } else {
                lines.push_back(tr("%1 — status temporarily unavailable").arg(QString::fromStdString(txid.ToString())));
            }
        }
        m_status->setText(lines.join("\n"));
    }
    if (m_preview_ready) return;
    const auto previously_selected = selectedCoins();
    m_refreshing = true;
    m_rows.clear();
    m_table->setRowCount(0);
    const auto coins = m_wallet_model->wallet().listCoins();
    for (const auto& [destination, group] : coins) {
        for (const auto& [outpoint, coin] : group) {
            const int expiry = coin.block_height < 0 ? -1 : coin.block_height + node::recycle::EXPIRY_BLOCKS;
            const bool eligible = coin.block_height >= 0 && coin.depth_in_main_chain > 0 && coin.blocks_to_maturity == 0 &&
                                  coin.is_spendable && coin.is_safe && !coin.is_spent &&
                                  !m_wallet_model->wallet().isLockedCoin(outpoint) && m_height < expiry;
            const int row = m_table->rowCount();
            m_table->insertRow(row);
            m_rows.push_back({outpoint, coin.txout.nValue, expiry, coin.input_bytes, eligible});
            auto* check = new QTableWidgetItem;
            check->setFlags(eligible ? Qt::ItemIsEnabled | Qt::ItemIsUserCheckable : Qt::NoItemFlags);
            const bool selected = std::any_of(previously_selected.begin(), previously_selected.end(), [&](const CoinRow& old) { return old.outpoint == outpoint; });
            check->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
            m_table->setItem(row, 0, check);
            m_table->setItem(row, 1, new QTableWidgetItem(formatAmount(coin.txout.nValue)));
            m_table->setItem(row, 2, new QTableWidgetItem(QString::number(coin.block_height)));
            m_table->setItem(row, 3, new QTableWidgetItem(QString::number(expiry)));
            const int remaining = expiry < 0 ? -1 : expiry - m_height;
            QString remaining_text;
            if (coin.block_height < 0 || coin.depth_in_main_chain <= 0) remaining_text = tr("Unconfirmed — not renewable");
            else if (coin.blocks_to_maturity > 0) remaining_text = tr("Immature (%1 blocks) — not renewable").arg(coin.blocks_to_maturity);
            else if (!coin.is_spendable || !coin.is_safe) remaining_text = tr("Not spendable — not renewable");
            else if (coin.is_spent) remaining_text = tr("Already spent");
            else if (m_wallet_model->wallet().isLockedCoin(outpoint)) remaining_text = tr("Locked — not renewable");
            else if (remaining <= 0) remaining_text = tr("Expired — not renewable");
            else {
                const double years = remaining / (6.0 * 24 * 365.25);
                remaining_text = tr("%1 blocks (~%2 years)").arg(remaining).arg(years, 0, 'f', 1);
            }
            m_table->setItem(row, 4, new QTableWidgetItem(remaining_text));
            m_table->setItem(row, 5, new QTableWidgetItem(QString::fromStdString(outpoint.ToString())));
        }
    }
    m_refreshing = false;
    selectionChanged();
}

std::vector<RenewUtxos::CoinRow> RenewUtxos::selectedCoins() const
{
    std::vector<CoinRow> result;
    for (int row = 0; row < m_table->rowCount() && row < static_cast<int>(m_rows.size()); ++row) {
        if (m_rows[row].eligible && m_table->item(row, 0) && m_table->item(row, 0)->checkState() == Qt::Checked) result.push_back(m_rows[row]);
    }
    return result;
}

CAmount RenewUtxos::estimatedFee(const std::vector<CoinRow>& coins, bool* used_fallback) const
{
    CAmount fee{0};
    bool fallback{false};
    wallet::CCoinControl control;
    control.m_allow_other_inputs = false;
    for (size_t offset = 0; offset < coins.size(); offset += MAX_INPUTS_PER_TRANSACTION) {
        const size_t count = std::min(MAX_INPUTS_PER_TRANSACTION, coins.size() - offset);
        unsigned int bytes = TX_OVERHEAD_VBYTES + P2WPKH_OUTPUT_VBYTES;
        for (size_t index = offset; index < offset + count; ++index) {
            bytes += coins[index].input_bytes > 0 ? coins[index].input_bytes : UNKNOWN_INPUT_VBYTES;
        }
        FeeReason reason{FeeReason::NONE};
        CAmount chunk_fee = m_wallet_model->wallet().getMinimumFee(bytes, control, nullptr, &reason);
        if (reason == FeeReason::FALLBACK || chunk_fee == 0) {
            chunk_fee = m_wallet_model->wallet().getInitialFallbackFee(bytes);
            fallback = true;
        }
        fee += chunk_fee;
    }
    if (used_fallback) *used_fallback = fallback;
    return fee;
}

void RenewUtxos::clearPreview()
{
    m_preview_ready = false;
    m_preview_destinations.clear();
    m_destination->setText(tr("New address: created during fee preview"));
    m_renew->setText(tr("Renew UTXOs — Preview"));
}

void RenewUtxos::selectionChanged()
{
    if (m_refreshing) return;
    clearPreview();
    const auto selected = selectedCoins();
    CAmount total{0};
    for (const auto& coin : selected) total += coin.amount;
    bool fallback{false};
    const CAmount fee = estimatedFee(selected, &fallback);
    m_summary->setText(selected.empty() ? tr("Select one or more eligible UTXOs.") :
        tr("Selected: %1 UTXO(s) · Renewed: %2 · Estimated fee: %3%4 · Final: %5")
            .arg(selected.size()).arg(formatAmount(total)).arg(formatAmount(fee))
            .arg(fallback ? " " + tr("(initial fallback)") : QString{})
            .arg(formatAmount(std::max<CAmount>(0, total - fee))));
    m_renew->setEnabled(!selected.empty() && total > fee);
}

void RenewUtxos::selectNearExpiry()
{
    for (int row = 0; row < m_table->rowCount(); ++row) if (m_rows[row].eligible) m_table->item(row, 0)->setCheckState(m_rows[row].expiry_height - m_height <= NEAR_EXPIRY_BLOCKS ? Qt::Checked : Qt::Unchecked);
}

void RenewUtxos::selectAllEligible()
{
    for (int row = 0; row < m_table->rowCount(); ++row) if (m_rows[row].eligible) m_table->item(row, 0)->setCheckState(Qt::Checked);
}

void RenewUtxos::renew()
{
    const auto selected = selectedCoins();
    if (selected.empty()) return;
    CAmount total{0};
    for (const auto& coin : selected) total += coin.amount;
    bool fallback{false};
    const CAmount estimate = estimatedFee(selected, &fallback);
    const size_t tx_count = (selected.size() + MAX_INPUTS_PER_TRANSACTION - 1) / MAX_INPUTS_PER_TRANSACTION;
    if (!m_preview_ready) {
        m_preview_destinations.clear();
        for (size_t i = 0; i < tx_count; ++i) {
            auto destination = m_wallet_model->wallet().getNewDestination(OutputType::BECH32, "Renewed UTXO");
            if (!destination) {
                QMessageBox::critical(this, tr("Wallet error"), tr("Could not create a fresh wallet address."));
                clearPreview();
                return;
            }
            m_preview_destinations.push_back(QString::fromStdString(EncodeDestination(*destination)));
        }
        QStringList destination_list;
        for (const auto& destination : m_preview_destinations) destination_list.push_back(destination);
        m_destination->setText(tx_count == 1 ? tr("New address: %1").arg(m_preview_destinations.front()) :
            tr("New addresses: %1 fresh addresses (one per transaction)\n%2").arg(tx_count).arg(destination_list.join("\n")));
        m_summary->setText(tr("Preview · Renewed: %1 · Estimated fee: %2%3 · Final: %4 · %5 transaction(s)")
            .arg(formatAmount(total)).arg(formatAmount(estimate))
            .arg(fallback ? " " + tr("(1 sat/vB initial fallback or higher node minimum)") : QString{})
            .arg(formatAmount(total - estimate)).arg(tx_count));
        m_status->setText(tr("Review the estimate and destination. Continue to unlock the wallet and see the exact fee before broadcast."));
        m_preview_ready = true;
        m_renew->setText(tr("Continue — Unlock & Confirm"));
        return;
    }

    WalletModel::UnlockContext unlock(m_wallet_model->requestUnlock());
    if (!unlock.isValid()) {
        m_status->setText(tr("Renewal cancelled; no transaction was sent."));
        return;
    }

    std::vector<std::unique_ptr<WalletModelTransaction>> transactions;
    CAmount exact_fee{0};
    unsigned int total_vsize{0};
    wallet::CCoinControl estimator_control;
    FeeReason actual_reason{FeeReason::NONE};
    const CAmount actual_estimate = m_wallet_model->wallet().getMinimumFee(1000, estimator_control, nullptr, &actual_reason);
    const bool use_actual_fallback = actual_reason == FeeReason::FALLBACK || actual_estimate == 0;
    for (size_t offset = 0, index = 0; offset < selected.size(); offset += MAX_INPUTS_PER_TRANSACTION, ++index) {
        const size_t end = std::min(selected.size(), offset + MAX_INPUTS_PER_TRANSACTION);
        CAmount amount{0};
        wallet::CCoinControl control;
        control.m_allow_other_inputs = false;
        control.m_min_depth = 1;
        if (use_actual_fallback) {
            control.m_feerate = CFeeRate{m_wallet_model->wallet().getInitialFallbackFee(1000)};
        }
        for (size_t i = offset; i < end; ++i) {
            control.Select(selected[i].outpoint);
            amount += selected[i].amount;
        }
        SendCoinsRecipient recipient(m_preview_destinations[index], tr("Renewed UTXO"), amount, {});
        recipient.fSubtractFeeFromAmount = true;
        auto transaction = std::make_unique<WalletModelTransaction>(QList<SendCoinsRecipient>{recipient});
        const auto status = m_wallet_model->prepareTransaction(*transaction, control);
        if (status.status != WalletModel::OK) {
            m_status->setText(tr("Transaction preparation failed (%1). Nothing was sent; refresh and try again.").arg(static_cast<int>(status.status)));
            return;
        }
        exact_fee += transaction->getTransactionFee();
        total_vsize += transaction->getTransactionSize();
        transactions.push_back(std::move(transaction));
    }

    if (exact_fee > COIN / 100 || (total > 0 && exact_fee * 100 > total)) {
        const auto choice = QMessageBox::warning(this, tr("High renewal fee"),
            tr("The exact fee, %1, is unusually high for %2 vB. Nothing has been sent. Continue only if you accept this fee.")
                .arg(formatAmount(exact_fee)).arg(total_vsize),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice != QMessageBox::Yes) {
            m_status->setText(tr("Renewal cancelled because of the high fee; no transaction was sent."));
            return;
        }
    }

    const double sat_per_vbyte = total_vsize == 0 ? 0.0 : static_cast<double>(exact_fee) / total_vsize;
    const QString question = tr("Spend %1 selected UTXO(s) to %2 fresh address(es)?\n\nAmount: %3\nActual size: %4 vB\nExact total fee: %5 (%6 sat/vB)\nFinal amount: %7\n\nThe new expiry starts at each transaction's confirmation block.")
        .arg(selected.size()).arg(tx_count).arg(formatAmount(total)).arg(total_vsize).arg(formatAmount(exact_fee))
        .arg(sat_per_vbyte, 0, 'f', 2).arg(formatAmount(total - exact_fee));
    SendConfirmationDialog confirm(tr("Confirm UTXO Renewal"), question, {}, {}, 3, true, false, this);
    if (confirm.exec() != QMessageBox::Yes) {
        m_status->setText(tr("Renewal cancelled; no transaction was sent."));
        return;
    }

    QStringList txids;
    try {
        for (auto& transaction : transactions) {
            const Txid txid = transaction->getWtx()->GetHash();
            m_wallet_model->sendCoins(*transaction);
            m_sent_txids.push_back(txid);
            txids.push_back(QString::fromStdString(txid.ToString()));
        }
    } catch (const std::exception& error) {
        m_status->setText(tr("Broadcast stopped: %1. Already listed TXIDs may have been sent; refresh before retrying.\n%2")
            .arg(QString::fromUtf8(error.what()), txids.join("\n")));
        refresh();
        return;
    }
    m_status->setText(tr("Broadcast; awaiting confirmation:\n%1").arg(txids.join("\n")));
    clearPreview();
    Q_EMIT coinsSent();
    refresh();
}
