#pragma once

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace exchange::test {

/// A unique filesystem path that removes itself and its SQLite sidecars.
///
/// Named with the pid and a counter rather than a fixed name, so tests can run
/// under `ctest -j` without colliding on the same file -- the same reason the
/// network tests bind port 0.
///
/// Cleanup covers `-wal` and `-shm` too: SQLite in WAL mode creates both
/// alongside the database, and leaving them behind makes a later run open a
/// database with someone else's journal attached.
class TempPath {
public:
    explicit TempPath(std::string label) {
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = counter.fetch_add(1, std::memory_order_relaxed);

        path_ =
            std::filesystem::temp_directory_path() /
            ("exchange-" + label + "-" + std::to_string(::getpid()) + "-" + std::to_string(unique));
        remove();
    }

    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    TempPath(TempPath&&) = delete;
    TempPath& operator=(TempPath&&) = delete;

    ~TempPath() { remove(); }

    [[nodiscard]] std::string str() const { return path_.string(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void remove() const noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

    std::filesystem::path path_;
};

} // namespace exchange::test
