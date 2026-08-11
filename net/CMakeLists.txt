# net -- epoll reactor, Session, length-prefixed binary codec, Publisher.
#        The only component that touches a socket.
add_library(exchange_net INTERFACE)
add_library(exchange::net ALIAS exchange_net)

target_link_libraries(exchange_net INTERFACE
    exchange::flags
    exchange::core
    exchange::concurrent)
