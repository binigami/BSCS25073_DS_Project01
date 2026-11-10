// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 50    01 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char* SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue {
    struct Node {
        T data;
        Node* next;
    };
     Node* front;
     Node* rear;
    int count;

public:
    Queue() {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }

    ~Queue() {
        while (!isEmpty()) {
            dequeue();
        }
    }

    void enqueue(const T& val) {
        Node* n = new Node();
         n->data = val;
        n->next = nullptr;
        if (rear == nullptr) {
            front = rear = n;
        }
        else {
            rear->next = n;
            rear = n;
        }
        count++;
    }

    T dequeue() {
         if (isEmpty()) return T();
       Node* temp = front;
         T val = temp->data;
        front = front->next;
        if (front == nullptr) {
            rear = nullptr;
        }
        delete temp;
        count--;
        return val;
    }
    bool isEmpty() {
        return count == 0;
    }

    int size() {
        return count;
    }
};
struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char* buf, size_t n)
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    size_t total  = 0;
    while (total < n) {
        ssize_t sent = send(sock, buf + total, n - total, 0);
        if (sent <= 0)  return false;
        total += sent;
    }
        return true;

}
bool sendError(int sock, const string& message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text.
    char status  = 1;
    uint32_t  crc = computeCRC32(&status, 1);
    crc =   computeCRC32(message.c_str(), message.length(), crc);
    crc = finalizeCRC32(crc);
    if (!sendAll(sock, &status, 1)) return false;
    if (!sendAll(sock, message.c_str(), message.length())) return false;
     if (!sendAll(sock, (const char*)&crc, 4)) return false;
     return true;
}
int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    int s =  socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        cout << "socket failed\n";
        exit(1);
    }
    int  opt = 1;
     setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
     addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        cout << "bind failed\n";
        exit(1);
    }
    if (listen(s, LISTEN_BACKLOG) < 0) {
        cout << "listen failed\n";
         exit(1);
    }
    return s;
}
int connectToServer(const char* host, int port){
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
     int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
     addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
     string cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
      int res = system(cmd.c_str());
    if (res == 0) return true;
    return false;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
     int sock = connectToServer(serverHost.c_str(), serverPort);
     if (sock >= 0) return sock;
    if (!startServer(serverPort)) {
        return -1;
    }
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0) return sock;
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo& c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    if (time(nullptr) - c.arrivedAt > 30) {
        sendError(c.sock, "client waited too long in queue");
        close(c.sock);
            return;
    }
     int serverSock = connectToServerWithWakeup();
    if (serverSock < 0) {
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }
      char buf[IO_BUFFER_SIZE];
    while (true) {
        ssize_t r = recv(c.sock, buf, IO_BUFFER_SIZE, 0);
        if (r <= 0)   break;
        if (!sendAll(serverSock, buf, r)) break;
    }
     shutdown(serverSock, SHUT_WR);
    while (true) {
        ssize_t r = recv(serverSock, buf, IO_BUFFER_SIZE, 0);
        if (r <= 0)  break;
        if (!sendAll(c.sock, buf, r)) break;
    }
        close(serverSock);
    close(c.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while (flag) {
         struct  sockaddr_in caddr;
        socklen_t len = sizeof(caddr);
        int csock = accept(listenSock, (struct sockaddr*)&caddr, &len);
        if (csock < 0) continue;
          char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &caddr.sin_addr, ip, INET_ADDRSTRLEN);
        ClientInfo info;
        info.sock =  csock;
        info.address = string(ip);
          info.port = ntohs(caddr.sin_port);
        info.arrivedAt = time(nullptr);
        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS) {
             queueMutex.unlock();
            sendError(csock, "server busy, try again later");
            close(csock);
        }
        else {
            pendingClients.enqueue(info);
             queueMutex.unlock();
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while (flag) {
        ClientInfo c;
        bool found =  false;
         queueMutex.lock();
        if (!pendingClients.isEmpty()) {
            c = pendingClients.dequeue();
            found = true;
        }
         queueMutex.unlock();
        if (found) {
             relayOneClient(c);
        }
        else {
            usleep(10000);
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char* argv[])
{
    if (argc >= 2) lbPort = atoi(argv[1]);
    if (argc >= 3) serverPort = atoi(argv[2]);
    signal(SIGINT, handler);
    int listenSock = createListenSocket(lbPort);
    thread t(acceptLoop, listenSock);
    t.detach();
    dispatcherLoop();
    close(listenSock);
    return 0;
}