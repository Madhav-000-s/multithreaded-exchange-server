#pragma once

#include "core/book.hpp"
#include "core/command.hpp"
#include "core/fill.hpp"

#include <vector>

namespace exchange {

/// What applying one command produced.
struct ApplyOutcome {
    std::vector<Fill> fills;

    /// False when the book declined the command -- an unknown order id, a
    /// zero-quantity amendment. Not an error: a normal outcome that the caller
    /// counts rather than throws on.
    bool accepted{false};

    /// Quantity that came to rest, for the acknowledgement.
    Quantity restingQty{0};
};

/// Applies one command to a book.
///
/// **The single path by which a command ever reaches a book**, and the reason
/// this is a free function rather than an Engine member.
///
/// Recovery replays the write-ahead log through *this*, exactly as the engine
/// thread does at run time. That is what makes recovery trustworthy: a second
/// implementation of "apply a command" would eventually disagree with the
/// first, and the disagreement would surface as a recovered book that differs
/// from the one that was logged -- discovered, if at all, long after the crash
/// that produced it.
///
/// Throws whatever the book throws (`InvalidOrderError` on a bad order). The
/// caller decides what that means: the engine counts a rejection and carries
/// on, recovery treats it as a divergence worth reporting.
[[nodiscard]] ApplyOutcome applyCommand(Book& book, Command&& command);

} // namespace exchange
