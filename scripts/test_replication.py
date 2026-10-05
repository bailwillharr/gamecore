#!/usr/bin/env python3
"""
Tests replication by running the multiplayer test game as several processes on this machine.

For each scenario a server is started along with a few bots (gamecore_template --bot). Some scenarios make the bots' connections
bad (packet loss, latency, jitter) using the link simulator that is built into the engine.

There are two kinds of scenario:
 - play:      the bots play the game, check that they see what a client should see (--test), and report the result with their
              exit code.
 - converge:  everyone plays for a while and then stops changing anything (--test-freeze). A few seconds later every process
              logs a digest of all of its replicated state. They must all be the same.

The server is either a dedicated server, or another bot that hosts the game (which tests a server that has a player of its own).

usage: test_replication.py [--game NAME] [build directory]
    --game NAME        gamecore_template (the default, with dedicated_server as its dedicated server) or ember_court
    build directory    default: out/build/x64-debug-windows or out/build/x64-debug-linux
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""
NUM_BOTS = 3
BASE_PORT = 47100
FREEZE_TIME = "8"

BAD = ["--sim-loss", "10", "--sim-latency", "60", "--sim-jitter", "20"]
TERRIBLE = ["--sim-loss", "25", "--sim-duplicate", "5", "--sim-latency", "100", "--sim-jitter", "40"]
# As much loss, but with a short round trip so that reliable messages are retried quickly enough to all arrive before the
# digests are taken
LOSSY = ["--sim-loss", "25", "--sim-duplicate", "5", "--sim-latency", "30", "--sim-jitter", "15"]

# (name, kind, server is a bot that hosts, link simulator options for the clients)
SCENARIOS = [
    ("play, perfect connection", "play", False, []),
    ("play, bad connection (10% loss, 60 +/- 20 ms latency)", "play", False, BAD),
    ("play, terrible connection (25% loss, 5% duplication, 100 +/- 40 ms latency)", "play", False, TERRIBLE),
    ("play, hosted by a player, bad connection", "play", True, BAD),
    ("converge, 25% loss", "converge", False, LOSSY),
    ("converge, hosted by a player, bad connection", "converge", True, BAD),
]


# For each game: the directory and name of the client, and the command line of a dedicated server (the port is added to it).
# The games take the same options (see their game.h).
GAMES = {
    "gamecore_template": (("gamecore_template", "gamecore_template"), ("dedicated_server", "dedicated_server", [])),
    "ember_court": (("ember_court", "ember_court"), ("ember_court", "ember_court", ["--server"])),
}


def parse_args():
    args = sys.argv[1:]
    game = "gamecore_template"
    if "--game" in args:
        i = args.index("--game")
        if i + 1 >= len(args) or args[i + 1] not in GAMES:
            sys.exit(f"--game needs one of: {', '.join(GAMES)}")
        game = args[i + 1]
        del args[i:i + 2]
    return game, (args[0] if args else None)


def find_build_dir(given):
    if given:
        return os.path.abspath(given)
    preset = "x64-debug-windows" if os.name == "nt" else "x64-debug-linux"
    return os.path.join(REPO, "out", "build", preset)


def interesting_lines(log_path):
    keywords = ("BOT TEST", "REPLICATION DIGEST", "Replication link", "[error]", "[critical]")
    try:
        with open(log_path, "r", errors="replace") as f:
            # drop the timestamp etc.
            return [line.rstrip().split("] ")[-1] for line in f if any(keyword in line for keyword in keywords)]
    except OSError:
        return ["(no log file)"]


def find_digest(lines):
    for line in lines:
        if "REPLICATION DIGEST" in line:
            return line.split("REPLICATION DIGEST ")[1]
    return None


def run_scenario(game, build_dir, log_dir, index, name, kind, bot_hosts, sim_args):
    (client_dir, client_name), (server_dir, server_name, server_options) = GAMES[game]
    server_exe = os.path.join(build_dir, server_dir, server_name + EXE)
    client_exe = os.path.join(build_dir, client_dir, client_name + EXE)
    port = str(BASE_PORT + index)
    test_args = ["--test"] if kind == "play" else ["--test-freeze", FREEZE_TIME]

    print(f"=== {name} ===", flush=True)

    server_log = os.path.join(log_dir, f"scenario{index}_server.log")
    if bot_hosts:
        server_args = [client_exe, "--host", port, "--bot"]
    else:
        server_args = [server_exe] + server_options + [port]
    server_args += ["--exit-when-empty", "--log", server_log]
    if kind == "converge":
        server_args += test_args
    server = subprocess.Popen(server_args, cwd=os.path.dirname(server_args[0]))

    bots = []
    for i in range(NUM_BOTS):
        log = os.path.join(log_dir, f"scenario{index}_bot{i}.log")
        args = [client_exe, "--connect", f"127.0.0.1:{port}", "--bot", "--log", log] + test_args + sim_args
        bots.append((subprocess.Popen(args, cwd=os.path.dirname(client_exe)), log))

    passed = True
    digests = []
    for i, (bot, log) in enumerate(bots):
        try:
            code = bot.wait(timeout=120)
        except subprocess.TimeoutExpired:
            bot.kill()
            code = -1
        lines = interesting_lines(log)
        for line in lines:
            print(f"  bot {i}: {line}")
        digests.append(find_digest(lines))
        if code != 0:
            print(f"  bot {i}: exit code {code}")
            passed = False

    # the server exits by itself once every bot has left
    try:
        code = server.wait(timeout=30)
        if code != 0:
            print(f"  server: exit code {code}")
            passed = False
    except subprocess.TimeoutExpired:
        server.kill()
        print("  server: didn't exit after the bots left")
        passed = False
    lines = interesting_lines(server_log)
    for line in lines:
        print(f"  server: {line}")
    digests.append(find_digest(lines))

    if kind == "converge":
        if None in digests or len(set(digests)) != 1:
            print("  the processes didn't all report the same state digest")
            passed = False

    print(f"=== {name}: {'PASS' if passed else 'FAIL'} ===", flush=True)
    return passed


def main():
    game, given_build_dir = parse_args()
    build_dir = find_build_dir(given_build_dir)
    log_dir = tempfile.mkdtemp(prefix="gamecore_replication_test_")
    print(f"game: {game}")
    print(f"build directory: {build_dir}")
    print(f"logs: {log_dir}")

    results = [run_scenario(game, build_dir, log_dir, i, *scenario) for i, scenario in enumerate(SCENARIOS)]

    print("ALL SCENARIOS PASSED" if all(results) else "SOME SCENARIOS FAILED")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
