#include "core/fill.hpp"

#include <ostream>

namespace exchange {

std::ostream& operator<<(std::ostream& os, const Fill& fill) {
    return os << "Fill{aggressor=" << fill.aggressorId << " resting=" << fill.restingId
              << " px=" << fill.price << " qty=" << fill.quantity
              << " side=" << (fill.aggressorSide == Side::Buy ? "buy" : "sell") << '}';
}

} // namespace exchange
