# Blocking socket: advanced qo'llanma

Bu fayl blocking socket tushunchasini, `recv()` ning blocking rejimdagi aniq semantikasini va real tizimlarda uchraydigan muammolarni advanced darajada yoritadi. Maqsad: o'qigan inson amaliy tarmoq ilovasini to'g'ri dizayn qila olishi.

## 0) Tezkor xulosa

Blocking socket - bu I/O chaqiruvlari kerakli shart bajarilmaguncha threadni uxlatib turadigan model. U sodda, lekin yuqori yuklama va ko'p ulanishlarda resurs sarfi keskin oshadi. `recv()` blocking rejimda data kelgunga qadar kutadi, lekin qaytishda doimo so'ralgan hajmni bermaydi. TCP oqim protokoli bo'lgani uchun framing (xabar chegaralari) ni ilova darajasida tashkil qilish shart.

## 1) Socket nima?

Socket - bu OS tomonidan beriladigan tarmoq endpointi. U file descriptor (yoki Windowsda handle) kabi ko'rinadi va `send()`/`recv()` chaqiruvlari bilan boshqariladi. Socketlar TCP (`SOCK_STREAM`) yoki UDP (`SOCK_DGRAM`) kabi protokollarga bog'lanadi va full-duplex aloqa qiladi.

### 1.1) TCP socket hayot sikli

- Server: `socket()` -> `bind()` -> `listen()` -> `accept()` -> `recv()`/`send()` -> `close()`
- Klient: `socket()` -> `connect()` -> `recv()`/`send()` -> `close()`

### 1.2) UDP socket hayot sikli

- `socket()` -> `bind()` (ixtiyoriy) -> `sendto()`/`recvfrom()` -> `close()`

## 2) Blocking socket nima? (socketdan farqi)

Socket - bu abstraksiya, blocking esa uning I/O rejimi. Farq shuki: socket tarmoq endpointini bildiradi, blocking rejim esa data yo'q bo'lsa chaqiruvlar threadni kutishga qo'yishini bildiradi.

**Ta'rif:** Blocking socket - bu soket I/O chaqiruvlari (`accept()`, `connect()`, `recv()`, `send()`) bajarilganda, shart bajarilmaguncha threadni to'xtatib turadigan soket. Chaqiruv tugamaguncha thread boshqa ish qilmaydi.

**Qachon bloklanadi?**
- `accept()` - yangi ulanish kelguncha.
- `connect()` - TCP qo'l siqish tugaguncha.
- `recv()` - data kelguncha yoki ulanish yopilguncha.
- `send()` - kernel send bufferi to'lganda joy bo'shaguncha.

**Default holat:** Ko'p OS va kutubxonalarda socketlar default blocking holatda. Non-blocking qilish uchun maxsus sozlama kerak (POSIX `fcntl`, Windows `ioctlsocket`).

**Oddiy mental model:**
- Thread -> I/O chaqiruv -> resurs tayyor bo'lmasa -> thread uxlaydi.
- Resurs tayyor bo'lganda -> thread uyg'onadi -> chaqiruv tugaydi.

### 2.1) Kernel darajasida nima sodir bo'ladi?

Blocking I/O - bu scheduler va kernel wait queue mexanizmlariga tayangan model. Asosiy oqimlar:

**Receive path:**
1. NIC paketni DMA orqali kernel ring bufferga joylaydi.
2. TCP/IP stack paketni qayta yig'adi va socket receive bufferiga qo'yadi.
3. Agar thread `recv()` da kutayotgan bo'lsa, kernel uni uyg'otadi.
4. Kernel receive bufferdan user bufferga copy qiladi va `recv()` qaytaradi.

**Send path:**
1. `send()` user bufferdan kernel send bufferga copy qiladi.
2. Agar send buffer to'lsa, thread bloklanadi.
3. ACK kelib, TCP window kengayganda joy bo'shaydi va thread uyg'onadi.

**Muhim ta'sirlar:**
- `SO_RCVBUF` va `SO_SNDBUF` o'lchami blocking ehtimolini oshirishi yoki kamaytirishi mumkin.
- TCP flow control (rwnd) va congestion control send/recv tezligiga ta'sir qiladi.
- Har uyg'otish scheduling va context switch xarajatini keltiradi.

Natijada thread CPU istemaydi, lekin u band deb hisoblanadi. Ko'p ulanishlarda bu model resurslar bosimini oshiradi.

### 2.2) Blocking vs non-blocking vs async (qisqa taqqoslash)

| Rejim              | Data yo'q bo'lsa | Qaytish xolati                 | Odatda qaysi mexanizm                 |
| ------------------ | ---------------- | ------------------------------ | ------------------------------------- |
| Blocking           | Thread kutadi    | Data kelganda qaytadi          | To'g'ridan-to'g'ri `recv()`/`send()`  |
| Non-blocking       | Kutmaydi         | `EAGAIN`/`EWOULDBLOCK`         | `fcntl`/`ioctlsocket`                 |
| Async/Event-driven | Thread band emas | Readiness event bilan ishlaydi | `select`/`poll`/`epoll`/`kqueue`/IOCP |

## 3) `recv()` blocking rejimda qanday ishlaydi?

`recv()` socketdan baytlarni o'qiydi. Blocking rejimda u shunday ishlaydi:

### 3.1) `recv()` qachon qaytadi?

Blocking `recv()` quyidagi holatlarda qaytadi:

- **Kamida 1 bayt mavjud bo'lsa.** (TCP oqim, minimal shart)
- **`SO_RCVLOWAT` sharti bajarilsa.** (kam ishlatiladi)
- **Peer graceful close qilsa.** `recv()` 0 qaytaradi.
- **Xatolik bo'lsa.** Masalan, `ECONNRESET` (RST), `ETIMEDOUT`.
- **Signal kelib, chaqiruv uzilsa.** POSIXda `EINTR`.
- **Timeout o'rnatilgan bo'lsa.** `SO_RCVTIMEO` tugaganda xatolik qaytadi.

### 3.2) Partial read va framing

TCP - bu oqim (stream). Shuning uchun:

- `recv()` so'ralgan baytlar sonini to'liq bermasligi odatiy hol.
- Xabar chegaralari saqlanmaydi. Ilova xabarlarni o'zi ajratadi.

**Praktik yechim:** length-prefix yoki delimiter-based framing ishlating.

### 3.3) Muhim flag va optionlar

- `MSG_WAITALL` - imkon qadar ko'p data to'plab qaytaradi, lekin baribir signal, xato yoki close bilan to'xtashi mumkin.
- `MSG_PEEK` - bufferdan o'qiydi, lekin chiqarib yubormaydi.
- `MSG_DONTWAIT` - shu chaqiruvni non-blocking qiladi.
- `SO_RCVTIMEO` - `recv()` uchun timeout.
- `SO_RCVLOWAT` - minimal qaytish chegarasi (kam ishlatiladi).

### 3.4) TCP va UDP farqi

**TCP:**
- Oqim protokoli, xabar chegaralari yo'q.
- `recv()` fragmentlangan data qaytarishi mumkin.
- 0 qaytishi - peer graceful close.

**UDP:**
- Datagram protokoli, xabar chegaralari saqlanadi.
- `recv()` bitta datagramni beradi.
- Buffer kichik bo'lsa, qolgan qismi tashlanadi (truncation).

### 3.5) Robust o'qish uchun minimal shablon (Python)

```python
def recv_exact(sock, n):
	data = bytearray()
	while len(data) < n:
		chunk = sock.recv(n - len(data))
		if not chunk:
			raise ConnectionError("peer closed")
		data.extend(chunk)
	return bytes(data)

# 4 bayt length-prefix protokol
raw_len = recv_exact(sock, 4)
msg_len = int.from_bytes(raw_len, "big")
if msg_len > 10_000_000:
	raise ValueError("message too large")
payload = recv_exact(sock, msg_len)
```

Bu yondashuv partial read muammosini bartaraf qiladi va framingni ilova darajasida aniq qiladi.

## 4) Blocking socket kamchiliklari (chuqurroq tahlil)

### 4.1) Skalalash va resource bosimi
- Har bir ulanish uchun thread/process kerak bo'ladi.
- Minglab threadlar context switch va stack memory bilan CPU va RAMni yemiradi.

### 4.2) Head-of-line blocking
- Bir ulanishdagi sekin klient butun threadni bloklaydi.
- Boshqa ishlar (logging, cache, DB) ham kechikishi mumkin.

### 4.3) Thundering herd va fairness
- Ko'p threadlar bir xil resursni kutsa, uyg'onganda CPU spike bo'lishi mumkin.
- Ba'zi ulanishlar resursni tezroq olib, boshqalarni siqib chiqaradi.

### 4.4) DoS va slowloris xavfi
- Klient juda sekin data yuborsa, thread uzoq band bo'ladi.
- Timeout va limitlar bo'lmasa, server resurslari tugaydi.

### 4.5) Timeout va shutdown murakkabligi
- Blocking chaqiruvlar cheksiz kutishi mumkin.
- To'g'ri shutdown logikasi bo'lmasa, threadlar osilib qoladi.

## 5) Kamchiliklarni yumshatish usullari

Blocking modeldan voz kechmasdan ham quyidagi defensive choralardan foydalaniladi:

- **Timeoutlar:** `SO_RCVTIMEO`, `SO_SNDTIMEO`.
- **Backpressure:** ilova darajasida request size limit qo'yish.
- **Keepalive:** `SO_KEEPALIVE` bilan o'lik ulanishlarni aniqlash.
- **Thread pool:** cheklangan miqdorda thread, navbat bilan ishlash.
- **Input validation:** framing, length limit, har bir mesaj uchun quota.
- **Graceful shutdown:** `shutdown(SHUT_WR)` bilan yarim yopish.

## 6) Qachon blocking socket ma'qul?

- Past trafikli servis yoki prototip.
- Oddiy komanda va tez ishlab chiqish kerak bo'lsa.
- Sistem resurslari yetarli va ulanish soni kichik bo'lsa.

## 7) Qachon boshqa yondashuv kerak?

- Yuqori yuklama (minglab parallel ulanishlar).
- Past latency va yuqori throughput kerak bo'lsa.
- Event-driven, async yoki I/O multiplexing zarur bo'lsa.

## 8) Amaliy checklist

- Protocol framing aniq bo'lsin (length-prefix yoki delimiter).
- `recv()` partial read bo'lishini hisobga oling.
- Timeoutlar va maksimal xabar hajmini belgilang.
- Graceful close va `recv()==0` holatini boshqaring.
- Ulanishlar soni oshsa, async modelga o'tish rejasini tayyorlang.

## 9) Real ishlatiladigan joylar va misollar

Blocking socketlar quyidagi holatlarda real tizimlarda uchraydi:

- **Ichki admin kanallar:** Masalan, faqat ichki tarmoqda ishlaydigan boshqaruv protokoli.
- **Past trafikli servislar:** Kichik mikroservislar yoki monitoring agentlari.
- **Embedded va IoT:** Uskuna bilan bitta-bitta ulanish, soddalik muhim bo'lganda.
- **Batch jarayonlar:** ETL yoki fayl uzatish bosqichlari (kechikish muhim emas).
- **CLI va diagnostika:** Telnet-uslubidagi oddiy komandalar.

### 9.1) Misol: length-prefix protokol (oddiy RPC)

**Kutilgan oqim:**

1. Klient 4 bayt uzunlik yuboradi (big-endian).
2. Keyin payload yuboradi.
3. Server `recv_exact` bilan to'liq o'qib, javob qaytaradi.

Bu model real tizimlarda ko'p ishlatiladi, chunki TCP oqim bo'lsa ham xabar chegaralari aniq bo'ladi.

### 9.2) Misol: line-based protokol (telnet uslubida)

**Kutilgan oqim:**

1. Klient `GET /status\r\n` kabi satr yuboradi.
2. Server `\r\n` gacha o'qib, komandani bajaradi.
3. Javob ham satr ko'rinishida qaytadi.

Bu model CLI va diagnostika protokollarida ishlatiladi.

### 9.3) Loyiha arxitektura ko'rinishlari (blocking)

- **Single-thread server:** bir vaqtning o'zida bitta klient, soddalik yuqori.
- **Thread-per-connection:** har ulanish uchun alohida thread, kichik sistemalarda ishlaydi.
- **Thread pool:** cheklangan threadlar navbat orqali ishlaydi, resurs nazorati yaxshiroq.
- **Prefork/process pool:** POSIX serverlarda process izolatsiyasi kerak bo'lganda.

Bu ko'rinishlar productionda faqat ulanishlar soni cheklangan, latency talablar yumshoq bo'lgan tizimlarda ishlatiladi.

## 10) C, C++, Python, Node.js da qanday foydalaniladi?

### 10.1) C (POSIX) - blocking odatda default

```c
#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

static ssize_t recv_exact(int fd, void *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		ssize_t r = recv(fd, (char *)buf + off, n - off, 0);
		if (r == 0) return 0; /* peer closed */
		if (r < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		off += (size_t)r;
	}
	return (ssize_t)off;
}

static ssize_t send_all(int fd, const void *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		ssize_t r = send(fd, (const char *)buf + off, n - off, 0);
		if (r < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		off += (size_t)r;
	}
	return (ssize_t)off;
}

/* length-prefix yuborish */
uint32_t len = htonl((uint32_t)msg_len);
send_all(fd, &len, sizeof(len));
send_all(fd, msg, msg_len);

/* length-prefix qabul qilish */
uint32_t net_len = 0;
if (recv_exact(fd, &net_len, sizeof(net_len)) <= 0) {
	/* close yoki error */
}
uint32_t payload_len = ntohl(net_len);
```

**Windows eslatma:** Winsockda `WSAStartup` kerak, `recv()` xatolari `WSAGetLastError()` orqali olinadi.

### 10.2) C++ - POSIX yoki `boost::asio` (sync)

```cpp
// boost::asio synchronous read/write blocking hisoblanadi
boost::asio::io_context io;
boost::asio::ip::tcp::socket sock(io);
boost::asio::connect(sock, endpoints);

boost::asio::write(sock, boost::asio::buffer(out_buf));
boost::asio::read(sock, boost::asio::buffer(in_buf));
```

**Izoh:** C++ da POSIX API ham ishlatiladi, lekin `boost::asio` sync funksiyalari blocking semantikani soddaroq beradi.

### 10.3) Python - `socket` default blocking

```python
import socket

def recv_exact(sock, n):
	data = bytearray()
	while len(data) < n:
		chunk = sock.recv(n - len(data))
		if not chunk:
			raise ConnectionError("peer closed")
		data.extend(chunk)
	return bytes(data)

sock = socket.create_connection(("127.0.0.1", 9000), timeout=5)
payload = b"ping"
sock.sendall(len(payload).to_bytes(4, "big") + payload)

raw_len = recv_exact(sock, 4)
msg_len = int.from_bytes(raw_len, "big")
reply = recv_exact(sock, msg_len)
```

**Izoh:** `sendall()` partial write muammosini yashiradi, `recv()` esa doimo partial bo'lishi mumkin.

### 10.4) Node.js - event loop, blocking yo'q

Node.js da `net` soketlari non-blocking event-driven. Blocking `recv()` yo'q, lekin protokolni buffering orqali boshqarasiz.

```js
const net = require("net");

net.createServer((socket) => {
  let buf = Buffer.alloc(0);
  socket.on("data", (chunk) => {
	buf = Buffer.concat([buf, chunk]);
	while (buf.length >= 4) {
	  const len = buf.readUInt32BE(0);
	  if (buf.length < 4 + len) break;
	  const payload = buf.slice(4, 4 + len);
	  buf = buf.slice(4 + len);
	  socket.write(Buffer.concat([Buffer.alloc(4), payload]));
	}
  });
}).listen(9000);
```

**Izoh:** Node da blocking model yo'q, shuning uchun `data` eventlari kelganida buffer yig'ib, xabar to'liq bo'lsa qayta ishlaysiz. Bu amaliy jihatdan `recv_exact` ga teng.

## 11) Amaliy kodlar va ishga tushirish

Quyidagi real-case kodlar yonidagi fayllarda berilgan:

- [1.c](1.c) - blocking TCP server/client (length-prefix framing, timeouts, thread-per-connection).
- [1.py](1.py) - blocking TCP server/client (thread-per-connection, framing, timeout).
- [1.js](1.js) - Node.js event-driven server/client (framing va backpressure).

**Ishga tushirish (namuna):**

```bash
# C
gcc -O2 -Wall -Wextra -pthread 1.c -o demo
./demo server 0.0.0.0 9000
./demo client 127.0.0.1 9000 "hello"

# Python
python 1.py server 0.0.0.0 9000
python 1.py client 127.0.0.1 9000 "hello"

# Node.js
node 1.js server 0.0.0.0 9000
node 1.js client 127.0.0.1 9000 "hello"
```

## Xulosa

Blocking socket modeli sodda va o'qitish uchun qulay, lekin real tizimlarda performance va scalability bo'yicha jiddiy cheklovlarga ega. `recv()` blocking rejimda data kelguncha threadni to'xtatadi, va TCP oqim protokoli bo'lgani uchun ilova framingni o'zi boshqarishi shart. Katta yuklamalarda esa non-blocking yoki async arxitektura ko'pincha yagona to'g'ri yo'l bo'ladi.
