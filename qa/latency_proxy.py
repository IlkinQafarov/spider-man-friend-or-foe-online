"""Private QA: a loopback TCP relay that behaves like an internet connection between the two test games.

The joiner connects to --listen; the relay connects on to the host at --target. Every chunk in each direction is held
for --delay-ms (one way, so the round trip is twice that) plus a random jitter of up to --jitter-ms, and every
--stall-every seconds one direction freezes for --stall-ms (a lost packet being resent). Order is kept, as on a real
reliable connection. Several sessions in a row are relayed (rejoin tests). Statistics go to --log as JSON lines.

Usage: python -I qa/latency_proxy.py --listen 49376 --target 49375 --delay-ms 60 --jitter-ms 20 --log proxy.jsonl
"""
import argparse
import asyncio
import json
import random
import socket
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", type=int, default=49376)
    parser.add_argument("--target", type=int, default=49375)
    parser.add_argument("--delay-ms", type=float, default=60.0)
    parser.add_argument("--jitter-ms", type=float, default=0.0)
    parser.add_argument("--stall-every", type=float, default=0.0, help="seconds between stalls (0 = none)")
    parser.add_argument("--stall-ms", type=float, default=0.0)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--log", required=True)
    args = parser.parse_args()
    if not (0 <= args.delay_ms <= 2000 and 0 <= args.jitter_ms <= 1000 and 0 <= args.stall_ms <= 5000):
        parser.error("delay 0..2000 ms, jitter 0..1000 ms, stall 0..5000 ms")
    rng = random.Random(args.seed)
    log = open(args.log, "a", encoding="utf-8")

    def event(name, **fields):
        log.write(json.dumps({"t": round(time.monotonic(), 3), "event": name, **fields}) + "\n")
        log.flush()

    async def pipe(reader, writer, direction, stats):
        queue = asyncio.Queue()
        release = time.monotonic()
        next_stall = time.monotonic() + args.stall_every if args.stall_every else None

        async def sender():
            while True:
                due, data = await queue.get()
                if data is None:
                    break
                wait = due - time.monotonic()
                if wait > 0:
                    await asyncio.sleep(wait)
                writer.write(data)
                await writer.drain()
            try:
                writer.write_eof()
            except OSError:
                pass

        task = asyncio.create_task(sender())
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                now = time.monotonic()
                due = now + (args.delay_ms + rng.uniform(-args.jitter_ms, args.jitter_ms)) / 1000.0
                if next_stall is not None and now >= next_stall:
                    due += args.stall_ms / 1000.0
                    next_stall = now + args.stall_every
                    stats["stalls"] += 1
                release = max(release, due)  # keep order like a reliable stream
                stats["chunks"] += 1
                stats["bytes"] += len(data)
                await queue.put((release, data))
        except (ConnectionError, OSError):
            pass
        await queue.put((0, None))
        await task

    async def handle(client_reader, client_writer):
        sock = client_writer.get_extra_info("socket")
        if sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        deadline = time.monotonic() + 60
        while True:
            try:
                host_reader, host_writer = await asyncio.open_connection("127.0.0.1", args.target)
                break
            except OSError:
                if time.monotonic() > deadline:
                    event("TARGET_UNREACHABLE")
                    client_writer.close()
                    return
                await asyncio.sleep(0.05)
        host_sock = host_writer.get_extra_info("socket")
        if host_sock:
            host_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        up = {"chunks": 0, "bytes": 0, "stalls": 0}
        down = {"chunks": 0, "bytes": 0, "stalls": 0}
        event("SESSION_START", delay_ms=args.delay_ms, jitter_ms=args.jitter_ms, stall_every=args.stall_every, stall_ms=args.stall_ms)
        await asyncio.gather(pipe(client_reader, host_writer, "joiner->host", up), pipe(host_reader, client_writer, "host->joiner", down))
        event("SESSION_END", joiner_to_host=up, host_to_joiner=down)
        for writer in (host_writer, client_writer):
            try:
                writer.close()
            except OSError:
                pass

    async def serve():
        server = await asyncio.start_server(handle, "127.0.0.1", args.listen)
        event("LISTENING", listen=args.listen, target=args.target)
        async with server:
            await server.serve_forever()

    try:
        asyncio.run(serve())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
