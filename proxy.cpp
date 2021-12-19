#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <map>
#include <iterator>
using namespace std;

#define MAX_CACHE_SIZE 1049000
#define MAX_REQUEST_SIZE 8192
#define MAX_RESPONSE_SIZE 102400
#define HOSTNAME_MAX_SIZE 512
#define PORT_MAX_SIZE 7
#define URI_MAX_SIZE 4096
#define VERSION_MAX_SIZE 8
#define HEADERS_MAX_SIZE 4096

// RequestInfo states
#define READ_REQUEST 0
#define SEND_REQUEST 1
#define READ_RESPONSE 2
#define SEND_RESPONSE 3

// Socket states
#define IS_READ 1
#define IS_WRITE 2

// Read states
#define READ_COMPLETE 0
#define READ_WAIT 1
#define READ_ERROR 2

// Send states
#define SEND_COMPLETE 0
#define SEND_WAIT 1
#define SEND_ERROR 2

#define MAXEVENTS 25
#define EVENT_SIZE 12
#define MAXLINE 25
#define LISTENQ 10
#define HEADERS_TERMINATOR "\r\n\r\n"

typedef struct {
    int clientSocket;
    int serverSocket;
    int state;
    char *buffer;
    int clientReadCount;
    int serverSendCount;
    int serverReadCount;
    int clientSendCount;
}RequestInfo;

map<int, RequestInfo *> RequestMap;


// Imagine network events as a binary signal pulse. 
// Edge triggered epoll only returns when an edge occurs, ie. transitioning from 0 to 1 or 1 to 0. 
// Regardless for how long the state stays on 0 or 1.
//
// Level triggered on the other hand will keep on triggering while the state stays the same.
// Thus you will read some of the data and then the event will trigger again to get more data
// You end up being triggered more to handle the same data
//
// Use level trigger mode when you can't consume all the data in the socket/file descriptor 
// and want epoll to keep triggering while data is available. 
//
// Typically one wants to use edge trigger mode and make sure all data available is read and buffered.
//
// HTTP Proxy Lab (I/O Multiplexing)

// In this lab, you will again be implementing an HTTP proxy that handles concurrent requests.  However, the server you produce this time will take advantage of I/O multiplexing.  Your server will not spawn any additional threads or processes (i.e., it will be single-threaded), and all sockets will be set to non-blocking.  While your server will not take advantage of multiprocessing, it will be more efficient by holding the processor longer because it is not blocking (and thus sleeping) on I/O.  Popular servers, such as NGINX, use a similar model (see https://www.nginx.com/blog/thread-pools-boost-performance-9x/).  This model is also referred to as an example of event-based programming, wherein execution of code is dependent on "events"--in this case the availability of I/O.

// Please read the epoll man page in preparation for this lab.  You will also want to have ready reference to several other man pages related to epoll.  Here is a list:
 
// epoll - general overview of epoll, including detailed examples
// epoll_create1 - shows the usage of the simple function to create an epoll instance
// epoll_ctl - shows the definition of the epoll_data and epoll_event structures, which are used by both epoll_ctl() and epoll_wait().  Also describes the event types with which events are registered to an epoll instance, e.g., for reading or writing, and which type of triggering is used (for this lab you will use edge-level triggering).
// epoll_wait - shows the usage of the simple epoll_wait() function, including how events are returned and how errors are indicated,
// Handout
// proxylab2-handout.tar  Download

// (Please note that the contents of this file are mostly the same as those used with the previous proxy lab, with the exception that the directory has been renamed to proxylab2-handout, so you don't accidentally overwrite your previous work!)

 
// Procedure
// The following is a general outline that might help you organize your server code:

// When you start up your HTTP server:
// Create an epoll instance with epoll_create1()
// Set up your listen socket (as you've done in previous labs), and configure it to use non-blocking I/O (see the man page for fcntl() for how to do this).
// Register your listen socket with the epoll instance that you created, for reading.
// Start an epoll_wait() loop, with a timeout of 1 second.
// In your epoll_wait() loop, you will do the following:
// If the result was a timeout (i.e., return value from epoll_wait() is 0), check if a global flag has been set by a handler and, if so, break out of the loop; otherwise, continue.
// If the result was an error (i.e., return value from epoll_wait() is less than 0), handle the error appropriately (see the man page for epoll_wait for more).
// If there was no error, you should loop through all the events and handle each appropriately.  See next bullet items.
// If an event corresponds to the listen socket, you should loop to accept() any and all client connections, configure each, in turn, to use non-blocking I/O (see the man page for fcntl() for how to do this), and register each returned client socket with the epoll instance that you created, for reading, using edge-triggered monitoring.  You will stop calling accept() when it returns a value less than 0.  If errno is set to EAGAIN or EWOULDBLOCK, then that is an indicator that there are no more clients currently waiting.
// If an event corresponds to the socket associated with a client request (client socket) or the socket associated with a the proxied request to an upstream server (server socket) , you should determine where you are in terms of handling the corresponding client request and begin (or resume) handling it.  You should only read() or write() on said socket if your event indicates that you can, and only until the read() or write() call returns a value less than 0.  In such cases (where a value less than 0 is returned), if errno is EAGAIN or EWOULDBLOCK, then that is an indicator that there is no more data to be read, or (for write) that the file descriptor is no longer available for writing.  See the Client Request Handling section for more information.
// After your epoll_wait() loop, you should clean up any resources (e.g., freeing malloc'd memory), and exit.

// Client Request Handling
// Just as with the previous parts of the proxy lab, your proxy will retrieve a requested from the upstream HTTP server.  The difference is that the socket you set up for communication with the server must now be set to non-blocking, just as the listening socket and the client socket are.  And you must register this socket with the epoll instance, for writing, using edge-triggered monitoring.

// For simplicity, you may wait to set the proxy-to-server socket as non-blocking after you call connect(), rather than before.  While that will mean that your server not fully non-blocking, it will allow you to focus on the more important parts of I/O multiplexing.  This is permissible.
 
// If you instead choose to set the socket as non-blocking before calling connect() (this is not required), you can execute connect() immediately, but you cannot initiate the write() call until epoll_wait() indicates that this socket is ready for writing; because the socket is non-blocking, connect() will return before the connection is actually set up.
 
// You will need to keep track of the "state" of reach request.  The reason is that, just like when using blocking sockets, you won't always be able to receive or send all your data with a single call to read() or write().  With blocking sockets in a multi-threaded server, the solution was to use a loop that received or sent until you had everything, before you moved on to anything else.  Because it was blocking, the kernel would context switch out the thread and put it into a sleep state until there was I/O.  However, with I/O multiplexing and non-blocking I/O, you can't loop until you receive (or send) everything; you have to stop when you get an value less than 0 and finish handling the other ready events, after which you will return to the epoll_wait() loop to see if it is ready for more I/O.  When a return value to read() or write() is less than 0 and errno is EAGAIN or EWOULDBLOCK, it is a an indicator that you are done for the moment--but you need to know where you should start next time it's your turn (see man pages for accept and read, and search for blocking).  For example, you'll need to associate with the request:
// the socket corresponding to the requesting client
// the socket corresponding to the connection to the Web server
// the current state of the request (see Client Request States)
// the buffer to read into and write from
// the total number of bytes read from the client
// the total number of bytes to write to the server
// the total number of bytes written to the server
// the total number of bytes read from the server
// the total number of bytes written to the client
// You can define a struct request_info (for example) that contains each of these members.

 
// Client Request States
// One way of thinking about the problem is in terms of "states".  The following is an example of a set of client request states, each associated with different I/O operations related to proxy operation:
// READ_REQUEST:
// This is the start state for every new client request.  You should initialize every new client request to be in this state.
// In this state, loop to read from the client socket until one of the following happens:
// you have read the entire HTTP request from the client.  If this is the case:
// parse the client request and create the request that you will send to the server.
// set up a new socket and use it to connect().
// configure the socket as non-blocking.
// register the socket with the epoll instance for writing.
// change state to SEND_REQUEST.
// read() (or recv()) returns a value less than 0 and errno is EAGAIN or EWOULDBLOCK.  In this case, don't change state; you will continue reading when you are notified by epoll that there is more data to be read.
// read() (or recv()) returns a value less than 0 and errno is something other than EAGAIN or EWOULDBLOCK.  This is an error, and you can cancel your client request and deregister your socket at this point.
// SEND_REQUEST:
// You reach this state only after the entire request has been received from the client and the connection to the server has been initiated (i.e., in the READ_REQUEST state).
// In this state, loop to write the request to the server socket until one of the following happens:
// you have written the entire HTTP request to the server socket.  If this is the case:
// register the socket with the epoll instance for reading.
// change state to READ_RESPONSE
// write() (or send()) returns a value less than 0 and errno is EAGAIN or EWOULDBLOCK.  In this case, don't change state; you will continue writing when you are notified by epoll that you can write again.
// write() (or send()) returns a value less than 0 and errno is something other than EAGAIN or EWOULDBLOCK.  This is an error, and you can cancel your client request and deregister your sockets at this point.
// READ_RESPONSE:
// You reach this state only after you have sent the entire HTTP request (i.e., in the SEND_REQUEST state) to the Web server.
// In this state, loop to read from the server socket until one of the following happens:
// you have read the entire HTTP response from the server.  Since this is HTTP/1.0, this is when the call to read() (recv()) returns 0, indicating that the server has closed the connection.  If this is the case:
// register the client socket with the epoll instance for writing.
// change state to SEND_RESPONSE.
// read() (or recv()) returns a value less than 0 and errno is EAGAIN or EWOULDBLOCK.  In this case, don't change state; you will continue reading when you are notified by epoll that there is more data to be read.
// read() (or recv()) returns a value less than 0 and errno is something other than EAGAIN or EWOULDBLOCK.  This is an error, and you can cancel your client request and deregister your socket at this point.
// SEND_RESPONSE:
// You reach this state only after you have received the entire response from the Web server (i.e., in the READ_RESPONSE state).
// In this state, loop to write to the client socket until one of the following happens:
// you have written the entire HTTP response to the client socket.  If this is the case:
// close your client socket.
// write() (or send()) returns a value less than 0 and errno is EAGAIN or EWOULDBLOCK.  In this case, don't change state; you will continue writing when you are notified by epoll that you can write again.
// write() (or send()) returns a value less than 0 and errno is something other than EAGAIN or EWOULDBLOCK.  This is an error, and you can cancel your client request and deregister your sockets at this point.
 
// Hints
// When planning your assignment, it will greatly help you to create a state machine that include all of the above client states and the conditions that trigger transitions from one state to another.  While there is some detail in the model above, this will help with your understanding of what is going on.
// As mentioned in previous labs, do not use the RIO code distributed with this and the previous proxy labs.  It was written for blocking sockets, and it will not work properly.
// Your code MUST compile and run properly (i.e., as tested by the driver) on the CS lab machines.  Note that you are still welcome to develop in another (Linux) environment (e.g., a virtual machine), but please compile and test on a CS lab machine before submission!
// Two notes on getaddrinfo():
// The typical use of getaddrinfo() is to call it and iterate through the linked list of addresses returned to find one that works.  If you choose (again, not required!) to make your proxy-to-server socket non-blocking before calling connect(), then the return value of connect() will not be truly be representative of whether or not you were able to connect().
// getaddrinfo() involves performing a DNS lookup, which is, effectively, I/O.  However, there is no asynchronous/non-blocking version of getaddrinfo(), so you may use it in synchronous/blocking fashion (For those interested in a more flexible alternative, see https://getdnsapi.net/).
// Because you are not sharing data across  threads, you shouldn't need to allocate any memory on the heap (e.g., using malloc or calloc); you can simply declare a large array of struct client_requests (for example) and then just keep track of which ones are being used, etc.
// Read the man pages - really.
// Reminders
// You may not use blocking I/O.  All sockets must be configured as non-blocking.  You need to set this up--it is not the default.  See instructions above.  There is one exception to this: you can wait to configure your proxy-to-server socket until after the call to connect().
// You may not use threads (i.e., with pthread_create()) or multiple processes (i.e., with fork()).
// You should not need or want to use semaphores, locks, or other components of synchronization.  You're welcome :)
 
// Testing
// Use the following command-line to test your proxy with the driver:

// ./driver.py -b 20 -c 75 -m 2 epoll
// ./driver.py -b 20 -c 75 epoll
// 
// Use curl command:
// curl --max-time 10 --proxy http://localhost:1234 http://localhost:1235/home.html
// Grading Breakdown
// 20 pts basic proxy operation
// 75 pts for handling concurrent requests (with epoll and non-blocking sockets)
// 3 pts for compilation without any warnings
// 2 pts for no memory leaks
// Note that the driver doesn't include the three points for checking for compiler warnings.

// Submission
// To submit, run "make handin" and upload the resulting tar file to LearningSuite.

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                    Globals                                                     //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
int g_edge_triggered = 1;
static const char *addHeaders = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:68.0) Gecko/20100101 Firefox/68.0\r\nConnection: close\r\nProxy-Connection: close\r\nAccept: */*\r\n\r\n";
int isDebug = 1;

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                     Debug                                                      //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
void print_bytes(unsigned char *bytes, int byteslen) {
	int i, j, byteslen_adjusted;

	if (byteslen % 8) {
		byteslen_adjusted = ((byteslen / 8) + 1) * 8;
	} else {
		byteslen_adjusted = byteslen;
	}

	for (i = 0; i < byteslen_adjusted + 1; i++) {
		if (!(i % 8)) {
			if (i > 0) {
				for (j = i - 8; j < i; j++) {
					if (j >= byteslen_adjusted) {
						fprintf(stderr, "  ");

					} else if (j >= byteslen) {
						fprintf(stderr, "  ");

					} else if (bytes[j] >= '!' && bytes[j] <= '~') {
						fprintf(stderr, " %c", bytes[j]);

					} else {
						fprintf(stderr, " .");
					}
				}
			}

			if (i < byteslen_adjusted) {
                if (i > 0) {
                    fprintf(stderr, "\n");
                }

				fprintf(stderr, "\t%02X: ", i);
			}
		} else if (!(i % 4)) {
			fprintf(stderr, " ");
		}

		if (i >= byteslen_adjusted) {
			continue;

		} else if (i >= byteslen) {
			fprintf(stderr, "   ");

		} else {
			fprintf(stderr, "%02X ", bytes[i]);
		}
	}

	fprintf(stderr, "\n");
}

void pbDebug(string name, unsigned char *bytes, int byteslen) {
    if (isDebug) {
        fprintf(stderr, "%s [\n", name.c_str());
        print_bytes(bytes, byteslen);
        fprintf(stderr, "]\n");
        fflush(stderr);
    }
}

void sDebug(string name, char *value) {
    if (isDebug) {
        fprintf(stderr, "%s: %s\n", name.c_str(), value);
        fflush(stderr);
    }
}

void iDebug(string name, int value) {
    if (isDebug) {
        fprintf(stderr, "%s: %d\n", name.c_str(), value);
        fflush(stderr);
    }
}

void debug(const char* msg) {
    if (isDebug) {
        fprintf(stderr, "%s\n", msg);
        fflush(stderr);
    }
}

void rmDebug() {
    for (auto const& x: RequestMap) {
        iDebug("x.first", x.first);
    }
}

void riDebug(string name, RequestInfo *requestInfo) {
    if (isDebug) {
        fprintf(stderr, "%s\n", name.c_str());
        iDebug("clientSocket", requestInfo->clientSocket);
        iDebug("serverSocket", requestInfo->serverSocket);
        iDebug("state", requestInfo->state);
        pbDebug("buffer", (unsigned char *)requestInfo->buffer, 128);
        iDebug("clientReadCount", requestInfo->clientReadCount);
        iDebug("serverSendCount", requestInfo->serverSendCount);
        iDebug("serverReadCount", requestInfo->serverReadCount);
        iDebug("clientSendCount", requestInfo->clientSendCount);
        fflush(stderr);
    }
}

void div(string div) {
    if (isDebug) {
        fprintf(stderr, "%s\n", div.c_str());
        fflush(stderr);
    }
}

void skip() {
    if (isDebug) {
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                  System Calls                                                  //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//

void close_fd(int fd) {
    div("//---------------------- close_fd -----------------------//");

    if (close(fd) == 0) {
        div("Close the fd given                                 [ PASS ]");
    } else {
        div("Close the fd given                               [ FAILED ]");
    }

    iDebug("fd", fd);

    skip();
}

void reset_buffer(char *buffer) {
    div("//-------------------- reset_buffer ---------------------//");
    char *bufferSave = buffer;
    char *bufferReturn = (char *)memset(buffer, 0, MAX_RESPONSE_SIZE);

    if (bufferSave == bufferReturn) {
        div("Buffer is reset to 0s                              [ PASS ]");
    } else {
        div("Buffer is reset to 0s                            [ FAILED ]");
    }

    skip();
}

void free_ri(RequestInfo *ri) {
    div("//----------------------- free_ri -----------------------//");

    riDebug("requestInfo", ri);

    // No return, never throws exceptions
    free(ri->buffer);
    debug("1");
    free(ri);
    debug("2");

    div("Free requestInfo->buffer                           [ PASS ]");
    div("Free requestInfo                                   [ PASS ]");
    skip();
}

char *find_str(const char *string, const char *substring) {
    char *occurance = strstr(string, substring);

    if (occurance == NULL) {
        div("Find substring in string                         [ FAILED ]");
        sDebug("string", (char *)string);
        sDebug("substring", (char *)substring);
        skip();
    }
}

void copy_to_str(char *dest, const char *src) {
    char *returnValue = strcpy(dest, src);

    if (returnValue != dest) {
        div("Copy string to destination                       [ FAILED ]");
        sDebug("string", src);
        sDebug("destination", dest);
        skip();
    }
}

// void copy_to_mem(void *dest, const void * src, size_t n) {

// }

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                     Legacy                                                     //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
int open_listenfd(char *port) {   
    int listenfd;
    int optval = 1;
    struct sockaddr_in ip4addr;

    ip4addr.sin_family = AF_INET;
    ip4addr.sin_port = htons(atoi(port));
    ip4addr.sin_addr.s_addr = INADDR_ANY;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }

    /* Eliminates "Address already in use" error from bind */
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    if (bind(listenfd, (struct sockaddr*)&ip4addr, sizeof(struct sockaddr_in)) < 0) {
        close_fd(listenfd);
        perror("bind error");
        exit(EXIT_FAILURE);
    }

    if (listen(listenfd, 100) < 0) {
        close_fd(listenfd);
        perror("listen error");
        exit(EXIT_FAILURE);
    }

    if (listenfd < 0) {
        printf("Unable to open listen socket on port %s\n", port);
        exit(1);
    }

    return listenfd;
}

int is_complete_request(char *buffer) {
    int out = strcmp(&buffer[strlen(buffer)-4], HEADERS_TERMINATOR) == 0 ? 1 : 0;

	return out;
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                Assemble Request                                                //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
void copy_to_buffer(const char *str, char *buffer, int *offset) {
    copy_to_str(buffer + *offset, str);

    *offset += strlen(str);
}

//------------------------------------------ Assemble Request to Server ------------------------------------------//
void assemble_request(char *buffer, char *hostname, char *port, char *uri, char *headers) {
	div("//--------------------------------- assemble_request ----------------------------------//");
    int use_sprintf = 0;

    if (use_sprintf) {
	    sprintf(buffer, "GET %s HTTP/1.0\r\nHost: %s\r\n%s", uri, hostname, headers);

    } else {
        const char *str1 = "GET ";
        const char *str2 = " HTTP/1.0\r\nHost: ";
        const char *str3 = "\r\n";
        int offset = 0;

        copy_to_buffer(str1, buffer, &offset);
        copy_to_buffer((const char *)uri, buffer, &offset);
        copy_to_buffer(str2, buffer, &offset);
        copy_to_buffer((const char *)hostname, buffer, &offset);
        copy_to_buffer(str3, buffer, &offset);
        copy_to_buffer((const char *)headers, buffer, &offset);
    }

    div("Assemble request                                                                 [ PASS ]");
    pbDebug("buffer", (unsigned char *)buffer, strlen(buffer));
    iDebug("requestSize", strlen(buffer));
    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                 Parse Request                                                  //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
//------------------------------------------------ Parse Hostname ------------------------------------------------//
void parse_hostname(const char *bufIn, char *hostname) {
	const char *delimiter1 = " http://";
	const char *delimiter2 = ":";
	const char *delimiter3 = "/";
	const char *bufHostname = strstr(bufIn, delimiter1) + strlen(delimiter1);
	int hostnameSize;
	
	// Calculate hostnameSize
	if (find_str(bufHostname, delimiter2)) {
		hostnameSize = find_str(bufHostname, delimiter2) - bufHostname;
	} else {
		hostnameSize = find_str(bufHostname, delimiter3) - bufHostname;
	}

	// Assign hostname
	memcpy(hostname, bufHostname, hostnameSize);
	*(hostname + hostnameSize) = '\0';

    div("Parse hostname                                                                   [ PASS ]");
    sDebug("hostname", hostname);
}

//-------------------------------------------------- Parse Port --------------------------------------------------//
void parse_port(const char *bufIn, char *port) {
	const char *delimiter1 = "GET http://";
	const char *delimiter2 = ":";
	const char *hostname = find_str(bufIn, delimiter1) + find_str(delimiter1);
	
	// If port is explicit
	if (strstr(hostname, delimiter2)) {
		// Calculate portSize
		const char *delimiter3 = "/";
		const char *bufPort = find_str(hostname, delimiter2) + find_str(delimiter2);
		const char *end = find_str(bufPort, delimiter3);
		const int portSize = end - bufPort;

		// Assign port
		memcpy(port, bufPort, portSize);
		*(port + portSize) = '\0';
	} else {
		memcpy(port, "80", 2);
	}

    div("Parse port                                                                       [ PASS ]");
    sDebug("port", port);
}

//-------------------------------------------------- Parse URI ---------------------------------------------------//
void parse_uri(const char *bufIn, char *uri) {
	const char *delimiter1 = " http://";
	const char *delimiter2 = "/";
	const char *delimiter3 = " ";
	const char *bufURI = find_str((find_str(bufIn, delimiter1) + strlen(delimiter1)), delimiter2);
	const char *end = find_str(bufURI, delimiter3);
	const int uriSize = end - bufURI;

	// Assign uri
	memcpy(uri, bufURI, uriSize);
	*(uri + uriSize) = '\0';

    div("Parse URI                                                                        [ PASS ]");
    sDebug("uri", uri);
}

void parse_headers(const char *bufIn, char *headers) {
	const int addHeadersSize = (find_str(addHeaders, "\r\n\r\n") + 4) - addHeaders;

	memcpy(headers, addHeaders, addHeadersSize);
	*(headers + addHeadersSize) = '\0';

    div("Parse headers                                                                    [ PASS ]");
    sDebug("headers", headers);
}

//------------------------------------------ Parse Request from Client -------------------------------------------//
void parse_request(const char *bufIn, char *hostname, char *port, char *uri, char *headers) {
	div("//---------------------------------- parse_request() ----------------------------------//");
    
    parse_hostname(bufIn, hostname);

    parse_port(bufIn, port);

    parse_uri(bufIn, uri);

    parse_headers(bufIn, headers);

    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                               Get Server Socket                                                //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
//--------------------------------------------- Add socket to flags ----------------------------------------------//
void add_socket_to_flags(int *socket) {
    int flags = fcntl(*socket, F_GETFL, 0);
    
    // Sets socket to non-blocking
    flags |= O_NONBLOCK;
    
    // Adds socket to fds to monitor
    fcntl(*socket, F_SETFL, flags);

    div("Add socket to flags                                                              [ PASS ]");
    iDebug("socket", *socket);
}

//------------------------------------------------ Assemble Hints ------------------------------------------------//
struct addrinfo assemble_hints() {
	struct addrinfo hints;

	// Allocate memory
	memset(&hints, 0, sizeof(struct addrinfo));

	// Set values
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	return hints;
}

//--------------------------------------------- Get an Address List ----------------------------------------------//
struct addrinfo *get_address_list(char *serverName, char *serverPort) {
	struct addrinfo hints = assemble_hints();
	struct addrinfo *addressList;
	int status = getaddrinfo(serverName, serverPort, &hints, &addressList);

	if (status != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
		exit(EXIT_FAILURE);
	}

	return addressList;
}

//--------------------------------------------- Get a Server Socket ----------------------------------------------//
void connect_with_serverSocket(int *serverSocket, char *hostname, char *port) {
    div("//----------------------------- connect_with_serverSocket -----------------------------//");
    struct addrinfo *addressList = get_address_list(hostname, port);
	struct addrinfo *address;

	// Iterates through addressList: if valid, establish connection
	for (address = addressList; address != NULL; address = address->ai_next) {
		// Establishes local socket
		*serverSocket = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		
		// If invalid, continue to next address
		if (*serverSocket == -1)
			continue;

		// Establishes connection
		if (connect(*serverSocket, address->ai_addr, address->ai_addrlen) == -1) {
			// If invalid, close socket
			close_fd(*serverSocket);

		} else {
			// If valid, break for
			break;
		}
	}

	// If no valid address, exit
	if (address == NULL) {
		div("Connect with serverSocket                                                      [ FAILED ]");
        iDebug("serverSocket", *serverSocket);
		exit(EXIT_FAILURE);

	} else {
        div("Connect with serverSocket                                                        [ PASS ]");
        iDebug("serverSocket", *serverSocket);
    }

	// Frees addressList
	freeaddrinfo(addressList);

    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                    Readers                                                     //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
//------------------------------------------- Read Request from Client -------------------------------------------//
int read_client_request(int clientSocket, char *buffer, int *sumRead) {
    div("//-------------------------------- read_client_request --------------------------------//");
    int continueRead = 1;
    int readStatus = -1;

    do {
        // Read request
        int readCount = read(clientSocket, &buffer[*sumRead], MAX_REQUEST_SIZE);
        
        // Increment sumRead
        if (readCount > 0) {
            *sumRead += readCount;
        }

        // Calculate readStatus
        if (is_complete_request(buffer)) {
            readStatus = READ_COMPLETE;

            buffer[*sumRead] = '\0';

            div("Read request from client                                                         [ PASS ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumRead);
            iDebug("sumRead", *sumRead);

            continueRead = 0;

        } else if (readCount < 0 && (errno & EAGAIN || errno & EWOULDBLOCK)) {
            readStatus = READ_WAIT;

            div("Read request from client                                                      [ WAITING ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumRead);
            iDebug("sumRead", *sumRead);
            
            continueRead = 0;

        } else if (readCount < 0) {
            readStatus = READ_ERROR;

            div("Read request from client                                                        [ ERROR ]");
            iDebug("errno", errno);
            sDebug("error", strerror(errno));
            pbDebug("buffer", (unsigned char *)buffer, *sumRead);
            iDebug("sumRead", *sumRead);

            continueRead = 0;
        }
    } while (continueRead == 1);

    skip();

    return readStatus;
}

//------------------------------------------ Read Response from Server -------------------------------------------//
int read_server_response(int serverSocket, char *buffer, int *sumRead) {
    div("//------------------------------- read_server_response --------------------------------//");
    int continueRead = 1;
    int readStatus = -1;

    do {
        // Read response
        int readCount = read(serverSocket, &buffer[*sumRead], MAX_RESPONSE_SIZE);
        
        // Increment sumRead
        if (readCount > 0) {
            *sumRead += readCount;
        }

        // Calculate readStatus
        if (readCount == 0) {
            readStatus = READ_COMPLETE;

            buffer[*sumRead] = '\0';

            div("Read response from server                                                        [ PASS ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumRead);
            iDebug("sumRead", *sumRead);

            continueRead = 0;

        } else if (readCount < 0 && (errno & EAGAIN || errno & EWOULDBLOCK)) {
            readStatus = READ_WAIT;

            div("Read response from server                                                     [ WAITING ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumRead);
            iDebug("sumRead", *sumRead);
            
            continueRead = 0;

        } else if (readCount < 0) {
            readStatus = READ_ERROR;

            div("Read response from server                                                       [ ERROR ]");
            iDebug("errno", errno);
            sDebug("error", strerror(errno));
            pbDebug("buffer", (unsigned char *)buffer, *sumRead);
            iDebug("sumRead", *sumRead);

            continueRead = 0;
        }
    } while (continueRead == 1);

    skip();

    return readStatus;
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                    Senders                                                     //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
//-------------------------------------------- Send Request to Server --------------------------------------------//
int send_to_server(int serverSocket, char *buffer, int *sumSent) {
	div("//---------------------------------- send_to_server -----------------------------------//");
	int requestSize = strlen(buffer);
    int continueSend = 1;
    int sendStatus = -1;

	do {
        // Send request
		int sendCount = write(serverSocket, &buffer[*sumSent], requestSize);

        // Increment sumSent
		if (sendCount > 0) {
			*sumSent += sendCount;
		}

        // Calculate sendStatus
        if (*sumSent == requestSize) {
            sendStatus = SEND_COMPLETE;

            div("Send request to server                                                           [ PASS ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumSent);
            iDebug("sumSent", *sumSent);

            continueSend = 0;

        } else if (sendCount < 0 && (errno & EAGAIN || errno & EWOULDBLOCK)) {
            sendStatus = SEND_WAIT;

            div("Send request to server                                                        [ WAITING ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumSent);
            iDebug("sumSent", *sumSent);

            continueSend = 0;

        } else if (sendCount < 0) {
            sendStatus = SEND_ERROR;

            div("Send request to server                                                          [ ERROR ]");
            iDebug("errno", errno);
            sDebug("error", strerror(errno));
            pbDebug("buffer", (unsigned char *)buffer, *sumSent);
            iDebug("sumSent", *sumSent);

            continueSend = 0;
        }
	} while (continueSend == 1);

    skip();

    return sendStatus;
}

//------------------------------------------- Send Response to Client --------------------------------------------//
int send_to_client(int clientSocket, char *buffer, int *sumRead, int *sumSent) {
	div("//---------------------------------- send_to_client -----------------------------------//");
	int requestSize = strlen(buffer);
    int continueSend = 1;
    int sendStatus = -1;

	do {
        // Send request
		int sendCount = write(clientSocket, &buffer[*sumSent], *sumRead);

        // Increment sumSent
		if (sendCount > 0) {
			*sumSent += sendCount;
		}

        // Calculate sendStatus
        if (*sumSent == requestSize) {
            sendStatus = SEND_COMPLETE;

            div("Send response to client                                                          [ PASS ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumSent);
            iDebug("sumSent", *sumSent);

            continueSend = 0;

        } else if (sendCount < 0 && (errno & EAGAIN || errno & EWOULDBLOCK)) {
            sendStatus = SEND_WAIT;

            div("Send response to client                                                       [ WAITING ]");
            pbDebug("buffer", (unsigned char *)buffer, *sumSent);
            iDebug("sumSent", *sumSent);

            continueSend = 0;

        } else if (sendCount < 0) {
            sendStatus = SEND_ERROR;

            div("Send response to client                                                         [ ERROR ]");
            iDebug("errno", errno);
            sDebug("error", strerror(errno));
            pbDebug("buffer", (unsigned char *)buffer, *sumSent);
            iDebug("sumSent", *sumSent);

            continueSend = 0;
        }
	} while (continueSend == 1);

    skip();

    return sendStatus;
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                     Epoll                                                      //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
//---------------------------------------------- Add event to epoll ----------------------------------------------//
void add_event_to_epoll(int *epollfd, struct epoll_event *event) {
    div("//-------------------------------- add_event_to_epoll ---------------------------------//");
    if (epoll_ctl(*epollfd, EPOLL_CTL_ADD, event->data.fd, event) == 0) {
        div("Add event to epoll                                                               [ PASS ]");
        iDebug("event->data.fd", event->data.fd);
        pbDebug("event", (unsigned char *)event, EVENT_SIZE);
        skip();

    } else {
        div("Add event to epoll                                                             [ FAILED ]");
        iDebug("epollfd", *epollfd);
        iDebug("event->data.fd", event->data.fd);
        pbDebug("event", (unsigned char *)event, EVENT_SIZE);
        abort();
    }
}

void mod_event_in_epoll(int *epollfd, int socket, struct epoll_event *event) {
    div("//-------------------------------- mod_event_in_epoll ---------------------------------//");
    if (epoll_ctl(*epollfd, EPOLL_CTL_MOD, socket, event) == 0) {
        div("Mod event in epoll                                                               [ PASS ]");
        iDebug("socket", socket);
        pbDebug("event", (unsigned char *)event, EVENT_SIZE);
        skip();

    } else {
        div("Mod event in epoll                                                             [ FAILED ]");
        iDebug("errno", errno);
        sDebug("error", strerror(errno));
        iDebug("epollfd", *epollfd);
        iDebug("socket", socket);
        pbDebug("event", (unsigned char *)event, EVENT_SIZE);
        abort();
    }
}

void del_event_in_epoll(int *epollfd, struct epoll_event *event) {
    div("//-------------------------------- del_event_in_epoll ---------------------------------//");
    if (epoll_ctl(*epollfd, EPOLL_CTL_DEL, event->data.fd, event) == 0) {
        div("Del event in epoll                                                               [ PASS ]");
        iDebug("event->data.fd", event->data.fd);
        pbDebug("event", (unsigned char *)event, EVENT_SIZE);
        skip();

    } else {
        div("Del event in epoll                                                             [ FAILED ]");
        iDebug("epollfd", *epollfd);
        iDebug("event->data.fd", event->data.fd);
        pbDebug("event", (unsigned char *)event, EVENT_SIZE);
        abort();
    }
}

struct epoll_event *build_event(int socket, int state) {
    struct epoll_event *event = (struct epoll_event *)calloc(1, EVENT_SIZE);

    event->data.fd = socket;

    if (state == IS_READ) {
        if (g_edge_triggered) {
            event->events = EPOLLIN | EPOLLET;  
        } else {
            event->events = EPOLLIN;
        }
    } else {
        if (g_edge_triggered) {
            event->events = EPOLLOUT | EPOLLET;  
        } else {
            event->events = EPOLLOUT;
        }
    }

    return event;
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                 State Machine                                                  //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
void incrementState(int *state) {
    div("//---------------------------------- increment_state ----------------------------------//");
    
    *state = *state + 1;

    switch (*state) {
        case SEND_REQUEST:
            div("Set state to SEND_REQUEST                                                        [ PASS ]");

            break;
        
        case READ_RESPONSE:
            div("Set state to READ_RESPONSE                                                       [ PASS ]");

            break;
        
        case SEND_RESPONSE:
            div("Set state to SEND_RESPONSE                                                       [ PASS ]");

            break;
        
        default:
            fprintf(stderr, "Error: unrecognized state %d\n", *state);
            fflush(stderr);
    }

    skip();
}

void switch_key_socket(int oldKeySocket) {
    div("//--------------------------------- switch_key_socket ---------------------------------//");
    RequestInfo *oldRI = RequestMap[oldKeySocket];
    RequestInfo *newRI = (RequestInfo *)calloc(1, sizeof(RequestInfo));
    int newKeySocket;

    *newRI = *RequestMap[oldKeySocket];

    if (oldKeySocket == oldRI->clientSocket) {
        newKeySocket = oldRI->serverSocket;

    } else if (oldKeySocket == oldRI->serverSocket) {
        newKeySocket = oldRI->clientSocket;

    } else {
        debug("Error: unknown key socket");
        iDebug("oldKeySocket", oldKeySocket);
        skip();

        exit(1);
    }

    RequestMap[newKeySocket] = newRI;

    RequestMap.erase(oldKeySocket);

    div("Switch key socket                                                                [ PASS ]");
    iDebug("oldKeySocket", oldKeySocket);
    iDebug("newKeySocket", newKeySocket);
    skip();
    riDebug("requestInfo", RequestMap[newKeySocket]);
    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                              Handle READ_REQUEST                                               //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
void handle_read_request(int epollfd, struct epoll_event *event) {
    div("//-------------------------------------------------------------------------------------//");
    div("//                                                                                     //");
    div("//                                    READ_REQUEST                                     //");
    div("//                                                                                     //");
    div("//-------------------------------------------------------------------------------------//");

      /* We have data on the fd waiting to be read. Read and
	 display it. If edge triggered, we must read whatever data is available
	 completely, as we are running in edge-triggered mode
	 and won't get a notification again for the same data. 
	 */
	
    RequestInfo *ri = RequestMap[event->data.fd];
    int isFirst = 1;
    int clientReadStatus = read_client_request(ri->clientSocket, ri->buffer, &ri->clientReadCount); // Replace buff with request map

    if (!g_edge_triggered) {
        printf("Done & not edge_triggered\n");
    }

    if (ri->clientReadCount == 0) { // received 0 bytes
        if (isFirst == 1) {/* if we get 0 bytes the first time through, the socket has been closed */
            /* Closing the descriptor will make epoll remove it
            from the set of descriptors which are monitored. */
            close_fd(event->data.fd);
            printf("Closed file descriptor %d\n", event->data.fd);
        }

    } else {
        ri->buffer[ri->clientReadCount] = '\0';
        printf("Read: [%s]\n",  ri->buffer);
        skip();

        // If we find the end of message marker, respond
        switch (clientReadStatus) {
            case READ_COMPLETE:
                char hostname[HOSTNAME_MAX_SIZE];
                char port[PORT_MAX_SIZE];
                char uri[URI_MAX_SIZE];
                char headers[HEADERS_MAX_SIZE];

                parse_request(ri->buffer, hostname, port, uri, headers);

                reset_buffer(ri->buffer);

                assemble_request(ri->buffer, hostname, port, uri, headers);

                connect_with_serverSocket(&ri->serverSocket, hostname, port);

                add_socket_to_flags(&ri->serverSocket);

                add_event_to_epoll(&epollfd, build_event(ri->serverSocket, IS_WRITE));

                incrementState(&ri->state);
                
                switch_key_socket(ri->clientSocket);

                break;
            
            case READ_WAIT:
                debug("READ_WAIT");
                iDebug("clientSocket", ri->clientSocket);

                break;
            
            case READ_ERROR:
                debug("READ_ERROR");
                
                close_fd(ri->clientSocket);
                div("Close clientSocket                                                               [ PASS ]");
                iDebug("clientSocket", ri->clientSocket);

                break;
            
            default:
                fprintf(stderr, "Error: unrecognized clientReadStatus %d\n", clientReadStatus);
                fflush(stderr);
        }
    }

    isFirst = 0;
    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                              Handle SEND_REQUEST                                               //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
void handle_send_request(int epollfd, struct epoll_event *event) {
    div("//-------------------------------------------------------------------------------------//");
    div("//                                                                                     //");
    div("//                                    SEND_REQUEST                                     //");
    div("//                                                                                     //");
    div("//-------------------------------------------------------------------------------------//");

    RequestInfo *ri = RequestMap[event->data.fd];

    int sendStatus = send_to_server(ri->serverSocket, ri->buffer, &ri->serverSendCount);

    switch(sendStatus) {
        case SEND_COMPLETE:
            mod_event_in_epoll(&epollfd, ri->serverSocket, build_event(ri->serverSocket, IS_READ)); // Modifying the serverSocket event? Not the clientSocket?

            incrementState(&ri->state);

            reset_buffer(ri->buffer);

            break;

        case SEND_WAIT:
            debug("SEND_WAIT");
            iDebug("serverSocket", ri->serverSocket);

            break;
        
        case SEND_ERROR:
            debug("SEND_ERROR");
                
            close_fd(ri->clientSocket);
            div("Close clientSocket                                                               [ PASS ]");
            iDebug("clientSocket", ri->clientSocket);

            close_fd(ri->serverSocket);
            div("Close serverSocket                                                               [ PASS ]");
            iDebug("serverSocket", ri->serverSocket);

            break;
        
        default:
            fprintf(stderr, "Error: unrecognized sendStatus %d\n", sendStatus);
            fflush(stderr);
    }

    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                              Handle READ_RESPONSE                                              //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
void handle_read_response(int epollfd, struct epoll_event *event) {
    div("//-------------------------------------------------------------------------------------//");
    div("//                                                                                     //");
    div("//                                    READ_RESPONSE                                    //");
    div("//                                                                                     //");
    div("//-------------------------------------------------------------------------------------//");

    RequestInfo *ri = RequestMap[event->data.fd];

    int readStatus = read_server_response(ri->serverSocket, ri->buffer, &ri->serverReadCount);

    switch(readStatus) {
        case READ_COMPLETE:
            mod_event_in_epoll(&epollfd, ri->clientSocket, build_event(ri->clientSocket, IS_WRITE));

            incrementState(&ri->state);

            switch_key_socket(ri->serverSocket);

            close_fd(ri->serverSocket);

            break;

        case READ_WAIT:
            debug("READ_WAIT");
            iDebug("serverSocket", ri->serverSocket);

            break;
        
        case READ_ERROR:
            debug("READ_ERROR");
                
            close_fd(ri->clientSocket);
            div("Close clientSocket                                                               [ PASS ]");
            iDebug("clientSocket", ri->clientSocket);

            close_fd(ri->serverSocket);
            div("Close serverSocket                                                               [ PASS ]");
            iDebug("serverSocket", ri->serverSocket);

            break;
        
        default:
            fprintf(stderr, "Error: unrecognized readStatus %d\n", readStatus);
            fflush(stderr);
    }

    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                              Handle SEND_RESPONSE                                              //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
void handle_send_response(int epollfd, struct epoll_event *event) {
    div("//-------------------------------------------------------------------------------------//");
    div("//                                                                                     //");
    div("//                                    SEND_RESPONSE                                    //");
    div("//                                                                                     //");
    div("//-------------------------------------------------------------------------------------//");

    RequestInfo *ri = RequestMap[event->data.fd];

    int sendStatus = send_to_client(ri->clientSocket, ri->buffer, &ri->serverReadCount, &ri->clientSendCount);

    switch(sendStatus) {
        case SEND_COMPLETE:
            close_fd(ri->clientSocket);

            free_ri(ri);

            break;

        case SEND_WAIT:
            debug("SEND_WAIT");
            iDebug("clientSocket", ri->clientSocket);

            break;
        
        case SEND_ERROR:
            debug("SEND_ERROR");
                
            close_fd(ri->clientSocket);
            div("Close clientSocket                                                               [ PASS ]");
            iDebug("clientSocket", ri->clientSocket);

            close_fd(ri->serverSocket);
            div("Close serverSocket                                                               [ PASS ]");
            iDebug("serverSocket", ri->serverSocket);

            break;
        
        default:
            fprintf(stderr, "Error: unrecognized sendStatus %d\n", sendStatus);
            fflush(stderr);
    }

    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                             Handle New Connection                                              //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
//-------------------------------------------- Get clientIP and port ---------------------------------------------//
int get_clientIP_and_port(struct sockaddr_in *in_addr, socklen_t *addr_size, char *clientIP, char *port) {
    pbDebug("in_addr", (unsigned char *)in_addr, (int)*addr_size);
    
    int status = getnameinfo ((struct sockaddr *)in_addr, *addr_size,
                                clientIP, sizeof clientIP,
                                port, sizeof port,
                                NI_NUMERICHOST | NI_NUMERICSERV);

    // Extract clientIP and port manually
    // memcpy(port, (const void *)&in_addr->sin_port, sizeof in_addr->sin_port);
    // sscanf(clientIP, "%c.%c.%c.%c", (char *)(&in_addr->sin_addr), (char *)(&in_addr->sin_addr) + 1, (char *)(&in_addr->sin_addr) + 2, (char *)(&in_addr->sin_addr) + 3);

    if (status == 0) {
        div("Get clientIP and port                                                            [ PASS ]");
        sDebug("clientIP", clientIP);
        sDebug("port", port);

    } else {
        div("Get clientIP and port                                                          [ FAILED ]");
        iDebug("status", status);
        fprintf(stderr, "error: %s\n", gai_strerror(status));
        iDebug("AF_INET", (int)AF_INET);
        sDebug("clientIP", clientIP);
        sDebug("port", port);
    }

    return status;
}

//-------------------------------------- Add requestInfo to the requestMap ---------------------------------------//
void add_requestInfo_to_requestMap(int *connectionSocket) {
    RequestInfo *requestInfo = (RequestInfo *)calloc(1, sizeof(RequestInfo));
    requestInfo->clientSocket = *connectionSocket;
    requestInfo->serverSocket = -1;
    requestInfo->state = 0;
    requestInfo->buffer = (char *)calloc(MAX_RESPONSE_SIZE, sizeof(char));
    requestInfo->clientReadCount = 0;
    requestInfo->serverSendCount = 0;
    requestInfo->serverReadCount = 0;
    requestInfo->clientSendCount = 0;
    
    RequestMap[*connectionSocket] = requestInfo;
    div("Add requestInfo to RequestMap                                                    [ PASS ]");
}

//------------------------------------------- Handle a New Connection --------------------------------------------//
void handle_new_connection(int epollfd, struct epoll_event *ev) {
    div("//------------------------------- handle_new_connection -------------------------------//");
	struct sockaddr_in in_addr;
	socklen_t addr_size = sizeof(in_addr);

    while(1) {
        int connectionSocket = accept(ev->data.fd, (struct sockaddr *)(&in_addr), &addr_size);

        if (connectionSocket > 0) {
            div("Accept new connection                                                            [ PASS ]");
        } else {
            div("Accept new connection                                                          [ FAILED ]");
        }
        skip();

        add_socket_to_flags(&connectionSocket);
        add_event_to_epoll(&epollfd, build_event(connectionSocket, IS_READ));
        add_requestInfo_to_requestMap(&connectionSocket);
        
        break;
    }

    skip();
}

//----------------------------------------------------------------------------------------------------------------//
//                                                                                                                //
//                                                      Main                                                      //
//                                                                                                                //
//----------------------------------------------------------------------------------------------------------------//
int main(int argc, char **argv) {
    div("//-------------------------------------------------------------------------------------//");
    div("//                                                                                     //");
    div("//                                        Proxy                                        //");
    div("//                                                                                     //");
    div("//-------------------------------------------------------------------------------------//");
    int listenfd = open_listenfd(argv[1]);
    int epollfd = epoll_create1(0);
    struct epoll_event *events = (struct epoll_event*)calloc(MAXEVENTS, EVENT_SIZE);

    iDebug("listenfd", listenfd);
    iDebug("epollfd", epollfd);
    skip();

    //---------------------------------------------- Validate Input ----------------------------------------------//
    if (epollfd < 0) {
        printf("main: epoll_create returned %d\n", epollfd);
        exit(1);
    }

    if (argc < 2) {
        printf("Usage: %s portnumber\n", argv[0]);
        exit(1);
    }
    div("Validate input                                                                   [ PASS ]");

    //------------------------------------ Add listenfd to epoll for reading -------------------------------------//
    add_event_to_epoll(&epollfd, build_event(listenfd, IS_READ));
    skip();

    //---------------------------------------------- Handle Events -----------------------------------------------//
    while (1) {
        debug("Before epoll_wait()");
    	int eventCount = epoll_wait(epollfd, events, MAXEVENTS, -1);  // Block until some events happen, no timeout

        debug("After epoll_wait()");

        for (int i = 0; i < eventCount; i++) {  
            iDebug("i", i);
            iDebug("eventCount", eventCount);
            iDebug("events[i].events", events[i].events);

            // Evaluate events[i]
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                debug("main: an epoll error has occured on the fd");
                iDebug("events[i].data.fd", events[i].data.fd);
                skip();

                // Closing the fd removes from the epoll monitored list
                close_fd(events[i].data.fd);

                continue;

            } else if (events[i].data.fd == listenfd) {
                handle_new_connection(epollfd, &events[i]);

            } else if ((events[i].events & EPOLLIN) || (events[i].events & EPOLLOUT)) {
                iDebug("events[i].data.fd", events[i].data.fd);
                riDebug("RequestMap[events[i].data.fd]", RequestMap[events[i].data.fd]);
                iDebug("RequestMap[events[i].data.fd]->state", RequestMap[events[i].data.fd]->state);
                skip();

                switch(RequestMap[events[i].data.fd]->state) {
                    case READ_REQUEST:
                        handle_read_request(epollfd, &events[i]);
                        break;
                    
                    case SEND_REQUEST:
                        handle_send_request(epollfd, &events[i]);
                        break;
                    
                    case READ_RESPONSE:
                        handle_read_response(epollfd, &events[i]);
                        break;
                    
                    case SEND_RESPONSE:
                        handle_send_response(epollfd, &events[i]);
                        break;
                    
                    default:
                        debug("Error: unrecognized state");
                        iDebug("state", RequestMap[events[i].data.fd]->state);
                }

            } else {
                debug("Error: detected unknown event type");
                pbDebug("event", (unsigned char *)&events[i], EVENT_SIZE);
                skip();
            }
        }
    }
}