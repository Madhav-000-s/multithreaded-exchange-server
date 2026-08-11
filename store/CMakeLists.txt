# store -- WriteAheadLog, SqliteStore, Recovery.
#
# SQLite is not a dependency yet; it arrives in Phase 6. When it does it is
# linked PRIVATE so that no other component can reach the database handle,
# which is what keeps the persister thread's ownership of it exclusive.
add_library(exchange_store INTERFACE)
add_library(exchange::store ALIAS exchange_store)

target_link_libraries(exchange_store INTERFACE
    exchange::flags
    exchange::core)
