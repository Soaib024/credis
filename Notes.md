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

compile server and client
```
g++ -Wall -Wextra -O2 -g server.cpp -o server.out
g++ -Wall -Wextra -O2 -g client.cpp -o client.out
```

## The Event Loop & Non-blocking IO
There are 3 ways to deal with concurrent connections in server-side network programming. 
They are: forking, multi-threading, and event loops. Forking creates new processes for each
client connection to achieve concurrency. Multi-threading uses threads instead of processes.
An event loop uses polling and nonblocking IO and usually runs on a single thread. 
Due to the overhead of processes and threads, most modern production-grade software uses event loops for networking.


Instead of just doing things (reading, writing, or accepting) with fds, we use the poll operation 
to tell us which fd can be operated immediately without blocking. When we perform an IO operation 
on an fd, the operation should be performed in the nonblocking mode.

In blocking mode, read blocks the caller when there are no data in the kernel, write blocks when
the write buffer is full, and accept blocks when there are no new connections in the kernel queue.
In nonblocking mode, those operations either success without blocking, or fail with the errno 
EAGAIN, which means “not ready”. Nonblocking operations that fail with EAGAIN must be 
retried after the readiness was notified by the poll.

The poll is the sole blocking operation in an event loop, everything else must be nonblocking;
thus, a single thread can handle multiple concurrent connections. All blocking networking IO APIs, 
such as read, write, and accept, have a nonblocking mode. APIs that do not have a nonblocking mode, 
such as gethostbyname, and disk IOs, should be performed in thread pools, Also, timers must be 
implemented within the event loop since we can’t sleep waiting inside the event loop.

We’ll use the poll syscall since it’s slightly less code than the stateful epoll API. However,
the epoll API is preferable in real-world projects since the argument for the poll can become too
large as the number of fds increases.

## Command encoding
+------+-----+------+-----+------+-----+-----+------+
| nstr | len | str1 | len | str2 | ... | len | strn |
+------+-----+------+-----+------+-----+-----+------+

The nstr is the number of strings and the len is the length of the following string. Both are 32-bit integers.

## Response encoding
+-----+---------+
| res | data... |
+-----+---------+

The response is a 32-bit status code followed by the response string.