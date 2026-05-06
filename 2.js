const net = require("net");
const readline = require("readline");

const IDLE_TIMEOUT_MS = Number(process.env.IDLE_TIMEOUT_MS || 30000);
const MAX_LINE = 1024 * 1024;

function startProxyBad(listenHost, listenPort, upHost, upPort) {
    const server = net.createServer((client) => {
        console.log(`client connected: ${client.remoteAddress}:${client.remotePort}`);
        client.setTimeout(IDLE_TIMEOUT_MS);
        client.once("data", (req) => {
            if (req.length > MAX_LINE) {
                client.end();
                return;
            }

            const upstream = net.createConnection(upPort, upHost);
            upstream.once("connect", () => {
                console.log(`upstream connected: ${upHost}:${upPort}`);
                upstream.write(req);
            });

            upstream.once("data", (res) => {
                client.end(res);
                upstream.destroy();
            });

            const cleanup = () => {
                client.destroy();
                upstream.destroy();
            };
            client.on("error", cleanup);
            upstream.on("error", cleanup);
        });

        client.on("timeout", () => client.destroy());
    });

    server.listen(listenPort, listenHost, () => {
        console.log(`proxy-bad listening on ${listenHost}:${listenPort}`);
    });
}

function startProxy(listenHost, listenPort, upHost, upPort) {
    const server = net.createServer((client) => {
        console.log(`client connected: ${client.remoteAddress}:${client.remotePort}`);
        client.setTimeout(IDLE_TIMEOUT_MS);
        client.setNoDelay(true);

        const upstream = net.createConnection(upPort, upHost);
        upstream.setTimeout(IDLE_TIMEOUT_MS);
        upstream.setNoDelay(true);
        upstream.once("connect", () => {
            console.log(`upstream connected: ${upHost}:${upPort}`);
        });

        client.pipe(upstream);
        upstream.pipe(client);

        const cleanup = () => {
            client.destroy();
            upstream.destroy();
        };

        client.on("timeout", cleanup);
        upstream.on("timeout", cleanup);
        client.on("error", cleanup);
        upstream.on("error", cleanup);
    });

    server.listen(listenPort, listenHost, () => {
        console.log(`proxy listening on ${listenHost}:${listenPort} -> ${upHost}:${upPort}`);
    });
}

function startChat(host, port) {
    const rooms = new Map();

    function broadcast(roomId, sender, message) {
        const room = rooms.get(roomId);
        if (!room) return;

        for (const socket of room) {
            if (socket === sender || socket.destroyed) continue;
            const flushed = socket.write(message);
            if (!flushed) {
                sender.pause();
                socket.once("drain", () => sender.resume());
            }
        }
    }

    const server = net.createServer((socket) => {
        console.log(`chat client connected: ${socket.remoteAddress}:${socket.remotePort}`);
        socket.setEncoding("utf8");
        socket.setTimeout(IDLE_TIMEOUT_MS);
        socket.setNoDelay(true);

        let roomId = null;
        let username = null;
        let buffer = "";

        socket.write("Ismingizni kiriting:\n");

        socket.on("data", (chunk) => {
            buffer += chunk;
            if (buffer.length > MAX_LINE) {
                socket.destroy();
                return;
            }

            const lines = buffer.split("\n");
            buffer = lines.pop();

            for (const line of lines) {
                const trimmed = line.trim();
                if (!trimmed) continue;

                if (!username) {
                    username = trimmed;
                    socket.write("Xona nomini kiriting:\n");
                    continue;
                }

                if (!roomId) {
                    roomId = trimmed;
                    if (!rooms.has(roomId)) rooms.set(roomId, new Set());
                    rooms.get(roomId).add(socket);
                    broadcast(roomId, socket, `[${username} xonaga kirdi]\n`);
                    socket.write(`Xona #${roomId} ga xush kelibsiz!\n`);
                    continue;
                }

                broadcast(roomId, socket, `${username}: ${trimmed}\n`);
            }
        });

        socket.on("timeout", () => {
            socket.write("Timeout. Ulanish yopildi.\n");
            socket.destroy();
        });

        socket.on("close", () => {
            if (roomId && rooms.has(roomId)) {
                rooms.get(roomId).delete(socket);
                broadcast(roomId, socket, `[${username} chiqib ketdi]\n`);
                if (rooms.get(roomId).size === 0) rooms.delete(roomId);
            }
        });

        socket.on("error", () => socket.destroy());
    });

    server.listen(port, host, () => {
        console.log(`chat server listening on ${host}:${port}`);
        console.log(`test: nc ${host} ${port}`);
    });
}

function startChatClient(host, port) {
    const socket = net.createConnection({ host, port });
    socket.setEncoding("utf8");
    socket.setTimeout(IDLE_TIMEOUT_MS);

    socket.on("data", (chunk) => {
        process.stdout.write(chunk);
    });

    socket.on("timeout", () => socket.destroy());
    socket.on("error", (err) => console.error(err.message));

    socket.on("connect", () => {
        console.log(`connected to chat server ${host}:${port}`);
        const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        rl.on("line", (line) => socket.write(line + "\n"));
        rl.on("close", () => socket.end());
    });
}

function printUsage() {
    console.log("usage:");
    console.log("  node 2.js proxy-bad <listenHost> <listenPort> <upHost> <upPort>");
    console.log("  node 2.js proxy <listenHost> <listenPort> <upHost> <upPort>");
    console.log("  node 2.js chat <host> <port>");
    console.log("  node 2.js chat-client <host> <port>");
}

function main() {
    const [mode, a, b, c, d] = process.argv.slice(2);
    if (!mode) {
        printUsage();
        process.exit(1);
    }

    if (mode === "proxy-bad") {
        if (!a || !b || !c || !d) {
            printUsage();
            process.exit(1);
        }
        startProxyBad(a, Number(b), c, Number(d));
        return;
    }

    if (mode === "proxy") {
        if (!a || !b || !c || !d) {
            printUsage();
            process.exit(1);
        }
        startProxy(a, Number(b), c, Number(d));
        return;
    }

    if (mode === "chat") {
        if (!a || !b) {
            printUsage();
            process.exit(1);
        }
        startChat(a, Number(b));
        return;
    }

    if (mode === "chat-client") {
        if (!a || !b) {
            printUsage();
            process.exit(1);
        }
        startChatClient(a, Number(b));
        return;
    }

    printUsage();
    process.exit(1);
}

main();
