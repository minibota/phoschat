#!/usr/bin/env python3
"""Serve sample IRC conversations on loopback for manual PhosChat demos."""

import argparse
import asyncio
from datetime import datetime, timezone
import ssl
from uuid import uuid4


HOST = "127.0.0.1"
SERVER = "studio.local"
CAPABILITIES = frozenset({"message-tags", "server-time"})
GREETINGS = {
    "#lobby": (
        ("mira", "Morning! I put the desktop build on the shared test machine."),
        ("alex", "Just tried it. I can keep this room open while I check my messages."),
        ("sam", "Nice. Let's keep the release notes short and link to the full changelog."),
    ),
    "#desktop": (
        ("mira", "The mint accents read well on the dark theme."),
        ("alex", "The little contact window fits beside my editor."),
        ("mira", "{nick}, can you check the spacing in the message list?"),
    ),
    "#off-topic": (
        ("sam", "Does anyone have a good coffee recommendation for this week?"),
        ("alex", "Try the washed Colombia if you see it."),
    ),
}
CHANNEL_TOPICS = {
    "#lobby": (12, "Introductions and project updates"),
    "#desktop": (8, "Desktop design and client development"),
    "#off-topic": (5, "Coffee, hobbies and conversation"),
}


class DemoSession:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self.reply_prefix = uuid4().hex
        self.nick = ""
        self.has_user = False
        self.negotiating = False
        self.registered = False
        self.capabilities = set()
        self.joined = set()
        self.privately_greeted = False
        self.replies = 0

    def send(self, line):
        self.writer.write(line.encode("utf-8") + b"\r\n")

    def names(self, channel):
        self.send(f":{SERVER} 353 {self.nick} = {channel} :@mira +alex sam {self.nick}")
        self.send(f":{SERVER} 366 {self.nick} {channel} :End of NAMES list")

    def welcome(self):
        self.registered = True
        self.send(f":{SERVER} 001 {self.nick} :Welcome to the PhosChat Studio demo")
        self.send(f":{SERVER} 005 {self.nick} CHANTYPES=# CASEMAPPING=rfc1459 "
                  "PREFIX=(ov)@+ :are supported by this server")
        self.send(f":{SERVER} 376 {self.nick} :End of MOTD")

    def message(self, sender, target, text, message_id):
        tags = []
        if "message-tags" in self.capabilities:
            tags.append(f"msgid={message_id}")
        if "server-time" in self.capabilities:
            timestamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
            tags.append(f"time={timestamp.replace('+00:00', 'Z')}")
        prefix = "@" + ";".join(tags) + " " if tags else ""
        self.send(f"{prefix}:{sender}!demo@{SERVER} PRIVMSG {target} :{text}")

    def join(self, channel):
        if not channel.startswith("#") or channel in self.joined:
            return
        self.joined.add(channel)
        self.send(f":{self.nick}!phoschat@{SERVER} JOIN {channel}")
        self.names(channel)
        greeting = GREETINGS.get(channel, (("mira", "Welcome to {channel}!"),))
        for number, (sender, text) in enumerate(greeting, start=1):
            self.message(sender, channel, text.format(nick=self.nick, channel=channel),
                         f"studio-{channel[1:]}-{number}")
        if not self.privately_greeted:
            self.privately_greeted = True
            self.message("mira", self.nick, f"Hi {self.nick}! Did you get a chance to try the new build?",
                         "studio-dm-1")
            self.message("mira", self.nick, "I have the project room open in another window.",
                         "studio-dm-2")

    def handle(self, line):
        head, separator, trailing = line.partition(" :")
        fields = head.split()
        if not fields:
            return True
        command = fields.pop(0).upper()
        if command == "QUIT":
            return False
        if command == "PING":
            token = trailing if separator else " ".join(fields)
            self.send(f":{SERVER} PONG {SERVER} :{token}")
        if command == "CAP" and fields:
            subcommand = fields[0].upper()
            target = self.nick or "*"
            if subcommand == "LS":
                self.negotiating = True
                self.send(f":{SERVER} CAP {target} LS :{' '.join(sorted(CAPABILITIES))}")
            elif subcommand == "REQ":
                request = trailing if separator else " ".join(fields[1:])
                requested = set(request.split())
                if requested <= CAPABILITIES:
                    self.capabilities.update(requested)
                    self.send(f":{SERVER} CAP {target} ACK :{request}")
                else:
                    self.send(f":{SERVER} CAP {target} NAK :{request}")
            elif subcommand == "END":
                self.negotiating = False
        elif command == "NICK" and fields:
            nickname = fields[0]
            if self.registered:
                self.send(f":{self.nick}!phoschat@{SERVER} NICK :{nickname}")
            self.nick = nickname
        elif command == "USER":
            self.has_user = True
        elif command == "LIST" and self.registered:
            for channel, (users, topic) in CHANNEL_TOPICS.items():
                self.send(f":{SERVER} 322 {self.nick} {channel} {users} :{topic}")
            self.send(f":{SERVER} 323 {self.nick} :End of LIST")
        elif self.registered and fields:
            target = fields[0]
            if command == "JOIN":
                self.join(target)
            elif command == "PART" and target in self.joined:
                self.joined.remove(target)
                self.send(f":{self.nick}!phoschat@{SERVER} PART {target}")
            elif command == "NAMES" and target in self.joined:
                self.names(target)
            elif command == "PRIVMSG" and separator:
                self.replies += 1
                if target in self.joined:
                    self.message("mira", target, "Got it. Thanks for checking!",
                                 f"reply-{self.reply_prefix}-{self.replies}")
                elif target.casefold() in {"mira", "alex", "sam"}:
                    self.message(target, self.nick, "Thanks! I'll take a look this afternoon.",
                                 f"reply-{self.reply_prefix}-{self.replies}")
                else:
                    self.send(f":{SERVER} 401 {self.nick} {target} :No such demo nickname")
        if not self.registered and not self.negotiating and self.nick and self.has_user:
            self.welcome()
        return True

    async def run(self):
        try:
            while line := await self.reader.readline():
                if len(line) > 512 or not line.endswith(b"\r\n"):
                    break
                if not self.handle(line[:-2].decode("utf-8", errors="replace")):
                    break
                await self.writer.drain()
        except (ConnectionError, OSError, ValueError):
            pass  # A disconnected client or invalid frame ends this local session.
        finally:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except OSError:
                pass


async def serve(port, tls):
    async def connected(reader, writer):
        await DemoSession(reader, writer).run()

    server = await asyncio.start_server(connected, HOST, port, ssl=tls, limit=8192)
    async with server:
        address = server.sockets[0].getsockname()
        mode = "TLS" if tls else "plain TCP"
        print(f"PhosChat IRC demo listening on {address[0]}:{address[1]} ({mode})", flush=True)
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=16667, help="loopback port (default: 16667)")
    parser.add_argument("--cert", help="PEM server certificate for optional TLS")
    parser.add_argument("--key", help="PEM private key for optional TLS")
    args = parser.parse_args()
    if bool(args.cert) != bool(args.key):
        parser.error("--cert and --key must be given together")
    if not 0 <= args.port <= 65535:
        parser.error("--port must be in the range 0–65535")
    tls = None
    if args.cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(args.cert, args.key)
    try:
        asyncio.run(serve(args.port, tls))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
