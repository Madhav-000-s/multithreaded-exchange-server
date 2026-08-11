# concurrent -- ThreadSafeQueue<T> and ThreadPool. The boundary between the
#               many-producer network side and the single-consumer engine.
#
# Deliberately depends on nothing but exchange::flags: the queue is a generic
# container and must not know what an Order is.
add_library(exchange_concurrent INTERFACE)
add_library(exchange::concurrent ALIAS exchange_concurrent)

target_link_libraries(exchange_concurrent INTERFACE exchange::flags)
