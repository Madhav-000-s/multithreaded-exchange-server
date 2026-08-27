# store -- WriteAheadLog, SqliteStore, Recovery.
find_package(SQLite3 REQUIRED)

add_library(exchange_store STATIC
    wal_record.cpp
    write_ahead_log.cpp
    sqlite.cpp
    sqlite_store.cpp
    recovery.cpp)

add_library(exchange::store ALIAS exchange_store)

target_link_libraries(exchange_store PUBLIC
    exchange::flags
    exchange::core)

# PRIVATE include path, deliberately: a dependent does not inherit it, so no
# other component picks up SQLite by accident -- reaching it takes a
# deliberate include rather than happening for free. CMake still propagates
# the link requirement for a static library, so dependents resolve the symbols
# without gaining the headers.
#
# SYSTEM because SQLite is C and would not survive this project's warning set;
# -Wold-style-cast alone would end it.
target_link_libraries(exchange_store PRIVATE SQLite::SQLite3)
target_include_directories(exchange_store SYSTEM PRIVATE ${SQLite3_INCLUDE_DIRS})

message(STATUS "  sqlite     : ${SQLite3_VERSION}")
