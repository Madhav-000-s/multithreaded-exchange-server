#pragma once

#include "core/command.hpp"
#include "core/types.hpp"

namespace exchange {

/// What the engine needs from a durable log, and nothing more.
///
/// **Declared here, in core, and implemented in store.** The dependency runs
/// store to core, so the engine cannot name `WriteAheadLog` directly without
/// inverting the graph and making the matching engine depend on SQLite. An
/// interface that core owns lets it state its requirement while store supplies
/// it -- dependency inversion for a concrete reason rather than as a reflex.
///
/// The secondary benefit is that the engine is testable without a filesystem:
/// a fake log that counts appends, or one that throws on the fifth, is four
/// lines.
class ICommandLog {
public:
    ICommandLog() = default;
    ICommandLog(const ICommandLog&) = delete;
    ICommandLog& operator=(const ICommandLog&) = delete;
    ICommandLog(ICommandLog&&) = delete;
    ICommandLog& operator=(ICommandLog&&) = delete;
    virtual ~ICommandLog() = default;

    /// Makes the command durable according to the configured policy.
    ///
    /// @return the assigned sequence number.
    /// @throws StorageError if it could not be recorded. The caller must then
    ///         *not* apply the command: an unlogged mutation is exactly the
    ///         divergence between log and state that the log exists to
    ///         prevent, and it would survive into recovery undetected.
    virtual Sequence append(const Command& command) = 0;

    /// Forces everything written so far, regardless of policy.
    virtual void flush() = 0;
};

} // namespace exchange
