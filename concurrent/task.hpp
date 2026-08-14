#pragma once

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace exchange {

/// A move-only type-erased nullary callable.
///
/// `std::function` will not do: it requires its target to be *copyable*, and
/// the work this pool carries owns a `unique_ptr<Order>`. Copyability is a
/// requirement `std::function` imposes for the sake of its own copy
/// constructor, which nothing here ever calls -- so the cost is paid purely to
/// satisfy an interface, and it is paid by making the natural code not
/// compile.
///
/// C++23 answers this with `std::move_only_function`; under C++20 the twenty
/// lines below do the same job. The shape is the standard type-erasure
/// idiom: an abstract `Concept` declaring the operation, a `Model<F>` holding
/// one concrete callable, and a `unique_ptr` to the base -- which is exactly
/// what makes the whole thing move-only for free.
class Task {
public:
    Task() = default;

    /// Constrained so that a Task is not mistaken for a callable and wrapped
    /// in itself, which would otherwise hijack the move constructor.
    template <typename F>
        requires std::invocable<F&> && (!std::same_as<std::decay_t<F>, Task>)
    // NOLINTNEXTLINE(google-explicit-constructor)
    Task(F&& callable)
        : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(callable))) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    ~Task() = default;

    void operator()() const { impl_->call(); }

    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    struct Concept {
        Concept() = default;
        Concept(const Concept&) = delete;
        Concept& operator=(const Concept&) = delete;
        Concept(Concept&&) = delete;
        Concept& operator=(Concept&&) = delete;
        virtual ~Concept() = default;

        virtual void call() = 0;
    };

    template <typename F>
    struct Model final : Concept {
        explicit Model(F callable) : fn(std::move(callable)) {}

        void call() override { fn(); }

        F fn;
    };

    std::unique_ptr<Concept> impl_;
};

static_assert(!std::is_copy_constructible_v<Task>, "a task must not require a copyable target");
static_assert(std::is_nothrow_move_constructible_v<Task>);

} // namespace exchange
