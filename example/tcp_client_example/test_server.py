import socket

HOST = "0.0.0.0"
PORT = 8080

def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)

        print(f"[server] listening on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            with conn:
                print(f"[server] connection from {addr}")

                data = b""
                while True:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                    if b"\r\n\r\n" in data:
                        break

                print("----- request -----")
                print(data.decode(errors="ignore"))
                print("-------------------")

                response = (
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 12\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "hello world\n"
                )

                conn.sendall(response.encode("ascii"))
                print("[server] response sent\n")

if __name__ == "__main__":
    main()
