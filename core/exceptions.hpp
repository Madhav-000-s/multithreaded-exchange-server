#pragma once

#include "core/types.hpp"

#include <stdexcept>
#include <string>

namespace exchange {

/// Base of every error this system raises deliberately.
///
/// Rooted in std::runtime_error, not in a bare class, so that a caller who
/// knows nothing about the exchange still catches it with
/// `catch (const std::exception&)`. A thread entry point or a main() that
/// catches std::exception is the last line of defence, and an exception
/// hierarchy that sits outside it turns a handled failure into
/// std::terminate.
///
/// runtime_error rather than logic_error: these describe conditions
/// discovered from data arriving at run time -- a malformed frame, an account
/// short of funds -- not programming mistakes. A programming mistake is a
/// precondition violation, and those are asserts (see core/precondition.hpp),
/// because there is no sensible recovery for code that is simply wrong.
class ExchangeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A frame that could not be decoded: bad length, unknown version, truncated
/// payload. Raised by the codec in Phase 5.
class ProtocolError : public ExchangeError {
public:
    using ExchangeError::ExchangeError;
};

/// An order that decoded cleanly but cannot be accepted: zero quantity, a
/// price off the tick grid, an unknown symbol, a duplicate id.
///
/// Distinct from ProtocolError because the two have different consequences.
/// A protocol error means the byte stream is no longer trustworthy and the
/// session must be dropped; an invalid order means this one message is
/// rejected and the session continues.
class InvalidOrderError : public ExchangeError {
public:
    using ExchangeError::ExchangeError;
};

/// The account cannot cover the order.
///
/// Carries the numbers rather than only a message: the rejection has to be
/// reported back to the client as structured fields, and re-parsing them out
/// of a formatted string would be absurd. This is the reason to define an
/// exception type at all rather than reuse the base with a different message.
class InsufficientFundsError : public ExchangeError {
public:
    InsufficientFundsError(AccountId account, Quantity required, Quantity available)
        : ExchangeError("insufficient funds for account " + std::to_string(account) +
                        ": required " + std::to_string(required) + ", available " +
                        std::to_string(available)),
          account_(account),
          required_(required),
          available_(available) {}

    [[nodiscard]] AccountId account() const noexcept { return account_; }

    [[nodiscard]] Quantity required() const noexcept { return required_; }

    [[nodiscard]] Quantity available() const noexcept { return available_; }

private:
    AccountId account_;
    Quantity required_;
    Quantity available_;
};

/// A durability failure: the write-ahead log could not be appended to, or
/// fsync reported an error. Phase 6.
///
/// The only one of these that is not recoverable at the session level. If the
/// WAL cannot be written, the engine must stop accepting orders rather than
/// mutate state it cannot replay.
class StorageError : public ExchangeError {
public:
    using ExchangeError::ExchangeError;
};
/// A socket-level or reactor-level failure: bind refused, accept failed,
/// epoll registration rejected.
///
/// Distinct from ProtocolError, which describes a peer sending nonsense. This
/// one describes the local machine refusing to cooperate, and almost always
/// means the server cannot start rather than that one session must be dropped.
///
/// Not in the ARCHITECTURE section 4 list, which predates the network layer
/// being written. Added rather than folded into an existing type because the
/// handling genuinely differs: a ProtocolError drops a session and the server
/// carries on, a NetworkError during startup is fatal.
class NetworkError : public ExchangeError {
public:
    using ExchangeError::ExchangeError;
};

} // namespace exchange
