## Listening Socket & Connection Socket

Two types of socket handles:
- Listening sockets. Obtained by listening on an address.
- Connection sockets. Obtained by accepting a client connection from a listening socket.

## The relevant syscalls on Linux:
- bind(): Configure the listening address of a socket.
- listen(): Make the socket a listening socket.
- accept(): Return a client connection socket, when available.

## In pseudo code:

```
fd = socket()
bind(fd, address)
listen(fd)
while True:
    conn_fd = accept(fd)
    do_something_with(conn_fd)
    close(conn_fd)
```

## Connect From a Client
The connect() syscall is for initiating a TCP connection from the client side. Pseudo code:

```
fd = socket()
connect(fd, address)
do_something_with(fd)
close(fd)
```
The type of a socket (listening or connection) is determined after the listen() or connect() syscall.