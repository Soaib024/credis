#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>

const size_t k_max_msg = 4096;

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2
};

struct Conn {
    int fd = -1;
    uint32_t state = 0;

    //buffer for reading
    size_t rbuf_size = 0; // Current size of data in the buffer
    uint8_t rbuf[4 + k_max_msg];

    //buffer for writing
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[2 + k_max_msg];
};

static void state_req(Conn * conn);
static void state_res(Conn * conn);

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void do_something(int connfd){
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);

    if(n < 0){
        msg("read() error");
        return;
    }

    printf("client says: %s\n", rbuf);
    char wbuf[] = "world\n";
    write(connfd, wbuf, strlen(wbuf));
}


static int32_t read_full(int fd, char *buf, size_t n){
    while(n > 0){
        /*
        The read() syscall just returns whatever data is available in the kernel, 
        or blocks if there is none. It’s up to the application to handle insufficient
        data. The read_full() function reads from the kernel until it gets exactly n bytes.
        */
        ssize_t rv = read(fd, buf, n);
        if(rv <= 0){
            return -1; // error or unexpected EOF
        }

        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }

    return 0;
}

static int32_t write_full(int fd, const char *buf, size_t n){
    while(n > 0){
        /*
        The write() syscall may return successfully with partial data written if the kernel buffer is full,
        we must keep trying when the write() returns fewer bytes than we need.
        */
        ssize_t rv = write(fd, buf, n);
        if(rv <= 0){
            return -1; // error
        }

        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

/*
+-----+------+-----+------+--------
| len(4B) | msg1 | len(4B) | msg2 | more...
+-----+------+-----+------+--------
*/

static int32_t one_request(int connfd){
    // 4B for len
    char rbuf[4 + k_max_msg + 1];

    errno = 0;
    int32_t err = read_full(connfd, rbuf, 4);

    if(err){
        if(errno == 0){
            msg("EOF");
        }else{
            msg("read() error");
        }
        return err;
    }

    uint32_t len = 0;
    // read first 4B from the buffer to len variable, assume little endian
    memcpy(&len, rbuf, 4);

    if(len > k_max_msg){
        msg("to long");
        return -1;
    }

    err = read_full(connfd, &rbuf[4], len);
    if(err){
        msg("read() error");
        return err;
    }

    // do something
    rbuf[4 + len] = '\0';
    printf("client says: %s\n", &rbuf[4]);

    // reply using the same protocol
    const char reply[] = "world";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_full(connfd, wbuf, 4 + len);
}

static void fd_set_nb(int fd) {
    errno = 0;  // Clear errno before calling fcntl
    /*
    The fcntl (file control) function in Unix-like operating systems provides various operations
    to manipulate file descriptors. It is defined in the <fcntl.h> header file.

    int fcntl(int fd, int cmd, ..... );

    Parameters
        fd: The file descriptor to be operated on.
        cmd: The command specifying the operation to be performed.
        arg: An optional third argument, depending on the command.
    */
    int flags = fcntl(fd, F_GETFL, 0);  // Get the current file status flags
    if (errno) {
        die("fcntl error");  // Handle error if fcntl fails
        return;
    }

    flags |= O_NONBLOCK;  // Add the non-blocking flag

    errno = 0;  // Clear errno before calling fcntl again
    (void)fcntl(fd, F_SETFL, flags);  // Set the new flags
    if (errno) {
        die("fcntl error");  // Handle error if fcntl fails
    }
}

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn * conn){
    if(fd2conn.size() <= (size_t)conn->fd){
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd){
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if(connfd < 0){
        msg("accept() error");
        return -1;  // error
    }
    fd_set_nb(connfd);
    struct Conn * conn = (struct Conn *)malloc(sizeof(struct Conn));
    if(!conn){
        close(connfd);
        return -1;
    }

    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn, conn);
    return 0;
}



// REQ handler
static bool try_one_request(Conn *conn){
    if(conn->rbuf_size < 4){
        // not enough data in the buffer, will retry in the next iteration
        return false;
    }

    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if(len > k_max_msg){
        msg("to long");
        conn->state = STATE_END;
        return false;
    }

    if(4 + len > conn->rbuf_size){
        // not all data received in the buffer, will retry in the next iteration
        return false;
    }

    //got one request, do something with it
    printf("client says: %.*s\n", len, &conn->rbuf[4]);

    //generating echoing response
    memcpy(&conn->wbuf[0], &len, 4);
    memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
    conn->wbuf_sent = 4 + len;

    size_t remain = conn->rbuf_size - 4 - len;
    if(remain){
        /*
        Left shift the content by 4 + len
        frequent memmove is inefficient, need better handling for production code (maybe circular buffer or LL)
        */
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }

    conn->rbuf_size = remain;

    conn->state = STATE_RES;
    state_res(conn);

    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn * conn){
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    size_t rv = 0;
    do{
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    }while(rv < 0 && errno == EINTR); // retry if if rv < 0 && EINTR signal is raised, 

    if(rv < 0 && errno == EAGAIN){
        // When performing non-blocking I/O. EAGAIN means "there is no data available right now, try again later".
        return false;
    }

    if(rv < 0){
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }

    if(rv == 0){
        if(conn->rbuf_size > 0){
            msg("unexpected EOF");
        }else{
            msg("EOF");
        }

        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    /*
    Try to process requests one by one using pipelining.
    Pipelining is a technique where multiple requests are sent on a 
    single connection without waiting for the corresponding responses. 
    This is common in protocols like HTTP/1.1, where a client can send
    multiple requests back-to-back and the server processes them sequentially.
    */
    while(try_one_request(conn)){}
    return (conn->state == STATE_REQ);
}

static void state_req(Conn * conn){
    while(try_fill_buffer(conn)){}
}

// RES Handler
static bool try_flush_buffer(Conn *conn){
    ssize_t rv = 0;
    do{
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while(rv < 0 && errno == EINTR);

    if(rv < 0 && errno == EAGAIN){
        return false;
    }

    if(rv < 0){
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }

    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if(conn->wbuf_sent == conn->wbuf_size){
        // response was fully sent, change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }

    // still got some data in wbuf, could try to write again
    return true;
}

static void state_res(Conn *conn){
    while(try_flush_buffer(conn)){}
}

static void connection_io(Conn *conn){
    if(conn->state == STATE_REQ){
        state_req(conn);
    }else if(conn->state == STATE_RES){
        state_res(conn);
    }else{
        assert(0);
    }
}




int main(){
    /*
    The line creates a socket with the following parameters:
    - **AF_INET**: Address family for IPv4.
    - **SOCK_STREAM**: Type for TCP, indicating a connection-oriented stream socket.
    - **IPPROTO_TCP**: Protocol explicitly set to TCP.

    This initializes a TCP socket for IPv4 networking.
    */
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);


    /*
    The `setsockopt` function call is used to set options for a socket. Here's a breakdown:
    - **fd**: The file descriptor of the socket.
    - **SOL_SOCKET**: Level where the option is defined (socket level).
    - **SO_REUSEADDR**: Allows the socket to bind to an address that is in a `TIME_WAIT` state.
    - **&val**: Pointer to the option value.
    - **sizeof(val)**: Size of the option value.

    This allows a socket to be bound to an address that was previously used by a socket that is now closed.
    This is useful for servers that need to restart and bind to the same address without waiting for the previous connection to fully close.
    */
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));



    /*
   The code snippet breaks down as follows:
    - **`struct sockaddr_in addr = {};`**: Initializes a `sockaddr_in` structure to zero.
    - **`addr.sin_family = AF_INET;`**: Sets the address family to IPv4.
    - **`addr.sin_port = htons(1234);`**: Sets the port to `1234`, converted to network byte order.
    - **`addr.sin_addr.s_addr = htonl(INADDR_ANY);`**: Binds to all available interfaces (wildcard address `0.0.0.0`).
    - **`bind(fd, (const sockaddr *)&addr, sizeof(addr));`**: Binds the socket `fd` to the specified address and port.

    This code sets up the socket to listen on port 1234 on all network interfaces.
    */

   /*
   struct sockaddr_in {
    short           sin_family;     // AF_INET
    unsigned short  sin_port;       // port number, big endian
    struct in_addr  sin_addr;       // IPv4 address
    char            sin_zero[8];    // useless
    };
   */
   struct sockaddr_in addr = {};
   addr.sin_family = AF_INET;
   addr.sin_port = ntohs(1234);
   addr.sin_addr.s_addr = ntohl(0);
   int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));



   if(rv){
    die("bind()");
   }

    /*
    The line does the following:
    - **`fd`**: The file descriptor for the socket.
    - **`SOMAXCONN`**: Specifies the maximum length for the queue of pending connections (typically a large value defined by the system).

    This call sets the socket to listen for incoming connections, allowing the server to accept them.
    After the listen() syscall, the OS will automatically handle TCP handshakes and place established connections in a queue.
    The application can then retrieve them via accept(). The backlog argument is the size of the queue, 
    which in our case is SOMAXCONN. SOMAXCONN is defined as 128 on Linux,
    */
    rv = listen(fd, SOMAXCONN);

    if (rv) {
        die("listen()");
    }

    // a map of all client connections, keyed by fd
    std::vector<Conn *> fd2conn;

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    std::vector<struct pollfd> poll_args;
    while(true){
        poll_args.clear();
        struct pollfd pfd = {fd, POLL_IN, 0};
        poll_args.push_back(pfd);

        for(Conn *conn: fd2conn){
            if(!conn){
                continue;
            }

            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLL_IN: POLL_OUT;
            pfd.events == pfd.events | POLL_ERR;
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
        if(rv < 0){
            die("poll");
        }

        for(size_t i = 1; i < poll_args.size(); ++i){
            if(poll_args[i].revents){
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if(conn->state == STATE_END){
                    fd2conn[conn->fd] = NULL;
                    (void)close(conn->fd);
                    free(conn);
                }
            }
        }

        if(poll_args[0].revents){
            (void)accept_new_conn(fd2conn, fd);
        }
    }
    return 0;
  
}