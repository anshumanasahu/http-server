import socket
import time
import threading
import random

# Server configuration
HOST = '127.0.0.1'
HTTP_PORT = 8080
FTP_PORT = 21
SMTP_PORT = 25
IMAP_PORT = 143

def send_http():
    try:
        with socket.create_connection((HOST, HTTP_PORT), timeout=2) as s:
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            s.recv(1024)
    except Exception as e:
        pass

def send_ftp():
    try:
        with socket.create_connection((HOST, FTP_PORT), timeout=2) as s:
            s.recv(1024) # 220
            s.sendall(b"USER test\r\n")
            s.recv(1024) # 331
            s.sendall(b"PASS test\r\n")
            s.recv(1024) # 230
            s.sendall(b"QUIT\r\n")
            s.recv(1024) # 221
    except Exception as e:
        pass

def send_smtp():
    try:
        with socket.create_connection((HOST, SMTP_PORT), timeout=2) as s:
            s.recv(1024) # 220
            s.sendall(b"HELO localhost\r\n")
            s.recv(1024) # 250
            s.sendall(b"MAIL FROM:<test@test.com>\r\n")
            s.recv(1024) # 250
            s.sendall(b"QUIT\r\n")
            s.recv(1024) # 221
    except Exception as e:
        pass

def send_imap():
    try:
        with socket.create_connection((HOST, IMAP_PORT), timeout=2) as s:
            s.recv(1024) # * OK
            s.sendall(b"A001 CAPABILITY\r\n")
            s.recv(1024) # * CAPABILITY ... A001 OK
            s.sendall(b"A002 LOGOUT\r\n")
            s.recv(1024)
    except Exception as e:
        pass

def worker(protocol_func, delay):
    while True:
        protocol_func()
        time.sleep(delay)

def main():
    print(f"Starting Multi-Protocol Traffic Generator on {HOST}...")
    print("This will send traffic to HTTP (8080), FTP (21), SMTP (25), and IMAP (143)")
    print("Press Ctrl+C to stop.")
    
    threads = []
    
    # 20 concurrent HTTP clients
    for _ in range(20):
        t = threading.Thread(target=worker, args=(send_http, random.uniform(0.1, 0.5)))
        t.daemon = True
        threads.append(t)
        
    # 5 concurrent FTP clients
    for _ in range(5):
        t = threading.Thread(target=worker, args=(send_ftp, random.uniform(0.5, 1.5)))
        t.daemon = True
        threads.append(t)

    # 5 concurrent SMTP clients
    for _ in range(5):
        t = threading.Thread(target=worker, args=(send_smtp, random.uniform(0.5, 1.5)))
        t.daemon = True
        threads.append(t)

    # 5 concurrent IMAP clients
    for _ in range(5):
        t = threading.Thread(target=worker, args=(send_imap, random.uniform(0.5, 1.5)))
        t.daemon = True
        threads.append(t)
        
    for t in threads:
        t.start()
        
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Stopping traffic generator.")

if __name__ == "__main__":
    main()
