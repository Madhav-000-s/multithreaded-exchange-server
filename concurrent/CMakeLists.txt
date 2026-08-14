# concurrent -- ThreadSafeQueue<T>, Task and ThreadPool. The boundary between
#               the many-producer network side and the single-consumer engine.
#
# Deliberately depends on nothing but exchange::flags: the queue is a generic
# container and must not know what an Order is. That is what lets it be tested
# in isolation and reused for any message type.
add_library(exchange_concurrent STATIC
    thread_pool.cpp)

add_library(exchange::concurrent ALIAS exchange_concurrent)

target_link_libraries(exchange_concurrent PUBLIC exchange::flags)
