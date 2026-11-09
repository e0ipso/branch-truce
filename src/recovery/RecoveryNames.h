// Recovery names: the typed actions and mutation facts a journal records, and their names on disk.
#pragma once

#include <QString>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace branch_truce {

// What a journal record did. Every file command records one of the first
// twelve; the coordinator records the two mutation facts.
enum class RecoveryAction {
    Open,
    ReplaceDraft,
    Undo,
    Redo,
    ManualSave,
    DiscardToCheckpoint,
    DiscardToStaged,
    MarkResolvedReview,
    WriteOutcome,
    StageOutcome,
    Support,
    LoadLarge,
    MutationIntent,
    MutationObservation
};

// The repository change a mutation record is about.
enum class MutationKind { Publication, Staging };

// What inspection saw after a mutation: nothing was attempted, the
// destination is unchanged, it changed, or it cannot be told.
enum class MutationObservation { NotAttempted, ObservedUnchanged, ObservedChanged, Uncertain };

// An enum's names on disk. Reordering or renaming the enum keeps its stored
// meaning, and a name this build does not know is refused when read.
template <typename Enum, std::size_t Size> using NameTable = std::array<std::pair<Enum, std::string_view>, Size>;

template <typename Enum, std::size_t Size>
[[nodiscard]] QString nameOf(const NameTable<Enum, Size> &table, Enum value)
{
    for (const auto &[entry, name] : table)
        if (entry == value) return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
    return {};
}

template <typename Enum, std::size_t Size>
[[nodiscard]] std::optional<Enum> valueNamed(const NameTable<Enum, Size> &table, const QString &name)
{
    for (const auto &[entry, entryName] : table)
        if (name == QLatin1StringView(entryName.data(), static_cast<qsizetype>(entryName.size()))) return entry;
    return std::nullopt;
}

inline constexpr NameTable<RecoveryAction, 14> recoveryActionNames{{
    {RecoveryAction::Open, "open"},
    {RecoveryAction::ReplaceDraft, "replace-draft"},
    {RecoveryAction::Undo, "undo"},
    {RecoveryAction::Redo, "redo"},
    {RecoveryAction::ManualSave, "manual-save"},
    {RecoveryAction::DiscardToCheckpoint, "discard-to-checkpoint"},
    {RecoveryAction::DiscardToStaged, "discard-to-staged"},
    {RecoveryAction::MarkResolvedReview, "mark-resolved-review"},
    {RecoveryAction::WriteOutcome, "write-outcome"},
    {RecoveryAction::StageOutcome, "stage-outcome"},
    {RecoveryAction::Support, "support"},
    {RecoveryAction::LoadLarge, "load-large"},
    {RecoveryAction::MutationIntent, "mutation-intent"},
    {RecoveryAction::MutationObservation, "mutation-observation"},
}};

inline constexpr NameTable<MutationKind, 2> mutationKindNames{{
    {MutationKind::Publication, "publication"},
    {MutationKind::Staging, "staging"},
}};

inline constexpr NameTable<MutationObservation, 4> mutationObservationNames{{
    {MutationObservation::NotAttempted, "not-attempted"},
    {MutationObservation::ObservedUnchanged, "unchanged"},
    {MutationObservation::ObservedChanged, "changed"},
    {MutationObservation::Uncertain, "uncertain"},
}};

}
