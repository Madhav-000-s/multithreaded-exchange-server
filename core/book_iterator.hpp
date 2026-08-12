#pragma once

#include "core/order.hpp"
#include "core/precondition.hpp"
#include "core/price_level.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <iterator>
#include <type_traits>

namespace exchange {

/// Flattens a book side into a single sequence of orders, best price first and
/// in queue order within each price.
///
/// The book is two levels of container -- a map of prices to a list of orders
/// -- and this presents them as one range so that standard algorithms apply
/// directly. Without it, every analytic would hand-roll the same nested walk.
///
/// **Const and non-const are one template, not two classes.** The only thing
/// that differs is four typedefs, so writing them twice would mean every fix
/// has to land twice, and eventually one of them would not. `IsConst` selects
/// the underlying iterator types and the reference type; everything else is
/// shared verbatim.
///
/// **Forward, not bidirectional or random-access.** Forward is the strongest
/// category the storage can honour: the multi-pass guarantee holds because map
/// and list iterators are stable, but `--` would have to step back across a
/// level boundary onto the *previous* level's last order, and there is no way
/// to reach that from a level iterator without also holding the container.
/// Random access is worse still -- `it + n` cannot be O(1) when the elements
/// are spread across an unknown number of lists.
///
/// Claiming a stronger category than the storage can honour does not fail to
/// compile; it silently changes which algorithm is selected. `std::distance`
/// would return garbage instead of counting, and `std::sort` would compile and
/// corrupt memory. The tag is a promise, not a hint.
///
/// @tparam Levels  the book side's `std::map<Price, PriceLevel, Cmp>`.
/// @tparam IsConst whether this iterator yields `const Order&`.
template <typename Levels, bool IsConst>
class BookIteratorImpl {
    using LevelIterator =
        std::conditional_t<IsConst, typename Levels::const_iterator, typename Levels::iterator>;
    using OrderIterator =
        std::conditional_t<IsConst, PriceLevel::ConstIterator, PriceLevel::Iterator>;

public:
    // The five members std::iterator_traits looks for. Supplying them is what
    // makes this usable by the standard algorithms at all -- the traits are
    // how an algorithm asks "what do you yield, and what can I do to you?"
    using iterator_category = std::forward_iterator_tag;
    using value_type = Order;
    using difference_type = std::ptrdiff_t;
    using reference = std::conditional_t<IsConst, const Order&, Order&>;
    using pointer = std::conditional_t<IsConst, const Order*, Order*>;

    /// C++20 ranges reads this in preference to iterator_category. They agree
    /// here; they differ for iterators whose reference is a prvalue, where the
    /// old category has to be weakened but the concept need not be.
    using iterator_concept = std::forward_iterator_tag;

    /// Required by std::forward_iterator, which subsumes std::semiregular.
    /// A default-constructed iterator is singular -- comparable, not
    /// dereferenceable.
    BookIteratorImpl() = default;

    BookIteratorImpl(LevelIterator level, LevelIterator levelEnd)
        : level_(level), levelEnd_(levelEnd) {
        if (level_ != levelEnd_) {
            order_ = level_->second.begin();
            skipExhaustedLevels();
        }
    }

    /// Implicit iterator -> const_iterator, and deliberately not the reverse.
    /// This is what lets `cbegin()` compare against a non-const `end()`, and
    /// what a caller expects from any standard container.
    template <bool WasConst>
        requires(IsConst && !WasConst)
    // NOLINTNEXTLINE(google-explicit-constructor)
    BookIteratorImpl(const BookIteratorImpl<Levels, WasConst>& other)
        : level_(other.level_), levelEnd_(other.levelEnd_), order_(other.order_) {}

    // The end iterator holds a singular `order_`; there is no list for it to
    // point into. Dereferencing or advancing it is a caller error and already
    // undefined, and saying so here keeps -Wnull-dereference from reporting a
    // branch that the contract forbids. See core/precondition.hpp.

    [[nodiscard]] reference operator*() const noexcept {
        EXCHANGE_PRECONDITION(level_ != levelEnd_);
        return **order_;
    }

    [[nodiscard]] pointer operator->() const noexcept {
        EXCHANGE_PRECONDITION(level_ != levelEnd_);
        return order_->get();
    }

    BookIteratorImpl& operator++() noexcept {
        EXCHANGE_PRECONDITION(level_ != levelEnd_);
        ++order_;
        skipExhaustedLevels();
        return *this;
    }

    BookIteratorImpl operator++(int) noexcept {
        BookIteratorImpl previous = *this;
        ++*this;
        return previous;
    }

    /// The price of the level the iterator currently sits on.
    ///
    /// Not part of the iterator protocol, but the alternative is for every
    /// caller to ask the order for its resting price -- which returns an
    /// optional and would need unwrapping at each use even though a resting
    /// order always has one.
    ///
    /// Precondition: not the end iterator.
    [[nodiscard]] Price price() const noexcept { return level_->second.price(); }

    [[nodiscard]] friend bool operator==(const BookIteratorImpl& lhs,
                                         const BookIteratorImpl& rhs) noexcept {
        if (lhs.level_ != rhs.level_) {
            return false;
        }
        // Both sit on the same level. If that level is the end, `order_` is
        // singular on at least one side and must not be compared.
        if (lhs.level_ == lhs.levelEnd_) {
            return true;
        }
        return lhs.order_ == rhs.order_;
    }

private:
    /// Advances past levels whose queue is spent, so the iterator never rests
    /// on a position that cannot be dereferenced.
    ///
    /// A loop rather than a single step because a level could in principle be
    /// empty. The book erases empty levels, so in practice this iterates once;
    /// relying on that invariant here would make the iterator break the moment
    /// the invariant is relaxed.
    void skipExhaustedLevels() noexcept {
        while (level_ != levelEnd_ && order_ == level_->second.end()) {
            ++level_;
            order_ = (level_ == levelEnd_) ? OrderIterator{} : level_->second.begin();
        }
    }

    /// So the const conversion constructor can read a non-const instance.
    template <typename, bool>
    friend class BookIteratorImpl;

    LevelIterator level_{};
    LevelIterator levelEnd_{};
    OrderIterator order_{};
};

} // namespace exchange
