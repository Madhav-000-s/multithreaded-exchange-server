# net -- epoll reactor, Session, length-prefixed binary codec, Publisher.
#        The only component that touches a socket.
add_library(exchange_net STATIC
    protocol.cpp
    frame_assembler.cpp
    socket.cpp
    session.cpp
    reactor.cpp
    server.cpp)

add_library(exchange::net ALIAS exchange_net)

target_link_libraries(exchange_net PUBLIC
    exchange::flags
    exchange::core
    exchange::concurrent)
