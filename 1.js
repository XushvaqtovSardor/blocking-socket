

const net = require("net");
const readline = require("readline");

const MAX_MSG_SIZE = 1_000_000;
const IO_TIMEOUT_MS = 5000;

function writeFrame(socket, payload) {
    const header = Buffer.alloc(4);
    header.writeUInt32BE(payload.length, 0);
    return socket.write(Buffer.concat([header, payload]));
}

function parseFrames(buffer, onFrame) {
    let buf = buffer;
    while (buf.length >= 4) {
        const len = buf.readUInt32BE(0);
        if (len > MAX_MSG_SIZE) {
            return { buf, error: "too_large" };
        }
        if (buf.length < 4 + len) {
            break;
        }
        const payload = buf.slice(4, 4 + len);
        buf = buf.slice(4 + len);
        onFrame(payload);
    }
    return { buf, error: null };
}

function startServer(host, port) {
    const server = net.createServer((socket) => {
        socket.setTimeout(IO_TIMEOUT_MS);
        let buf = Buffer.alloc(0);

        socket.on("data", (chunk) => {
            buf = Buffer.concat([buf, chunk]);
            const result = parseFrames(buf, (payload) => {
                if (payload.length > 0) {
                    process.stdout.write(payload.toString("utf8") + "\n");
                }
                writeFrame(socket, Buffer.from("OK"));
            });

            buf = result.buf;
            if (result.error === "too_large") {
                writeFrame(socket, Buffer.from("ERR: message too large"));
                socket.destroy();
            }
        });

        socket.on("timeout", () => socket.destroy());
    });

    server.listen(port, host, () => {
        console.log(`listening on ${host}:${port}`);
    });
}

function startClient(host, port, message) {
    const socket = net.createConnection({ host, port });
    socket.setTimeout(IO_TIMEOUT_MS);
    let buf = Buffer.alloc(0);

    socket.on("data", (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        const result = parseFrames(buf, (payload) => {
            process.stdout.write(payload.toString("utf8") + "\n");
        });
        buf = result.buf;
    });

    socket.on("timeout", () => socket.destroy());
    socket.on("error", (err) => console.error(err.message));

    socket.on("connect", () => {
        if (message) {
            writeFrame(socket, Buffer.from(message));
            return;
        }

        const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        let paused = false;

        rl.on("line", (line) => {
            const payload = Buffer.from(line, "utf8");
            if (payload.length === 0) {
                return;
            }
            const ok = writeFrame(socket, payload);
            if (!ok && !paused) {
                paused = true;
                rl.pause();
                socket.once("drain", () => {
                    paused = false;
                    rl.resume();
                });
            }
        });

        rl.on("close", () => socket.end());
    });
}

function main() {
    const [mode, host, portStr, message] = process.argv.slice(2);
    if (!mode || !host || !portStr) {
        console.error("usage: node 1.js server <host> <port>");
        console.error("   or: node 1.js client <host> <port> [message]");
        process.exit(1);
    }
    const port = Number(portStr);
    if (mode === "server") {
        startServer(host, port);
    } else if (mode === "client") {
        startClient(host, port, message);
    } else {
        console.error("unknown mode");
        process.exit(1);
    }
}

main();
