#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

const size_t k_max_msg = 4096;

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

    /*
    Event Loop
    */
   while(true){
    /*
    The code snippet does the following:

    - **`struct sockaddr_in client_addr = {};`**: Initializes a `sockaddr_in` structure for the client's address.
    - **`socklen_t addrlen = sizeof(client_addr);`**: Sets the length of the address structure.
    - **`accept(fd, (struct sockaddr *)&client_addr, &addrlen);`**: 

    - Waits for an incoming connection on the listening socket `fd`.
    - Fills `client_addr` with the client's address information.
    - Returns a new file descriptor `connfd` for the established connection.

    This allows the server to handle the new connection from the client.
    */
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    /*
    struct sockaddr * is the type used by the socket API, the structure itself is useless. Just cast any relevant structure to this type.
    */
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);

    if(connfd < 0){
        continue;
    }

    /*
        Only support one request at a time
    */
    while(true){
        int32_t err = one_request(connfd);
        if(err){
            break;
        }
    }
    close(connfd);
   }
   return 0;
  
}