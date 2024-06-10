#!/usr/bin/env python3
import argparse
import subprocess
import sys
import signal
import time

# Global variable to store if we should pause
pause_processing = False
total = 0
success = 0
failed = 0


def signal_handler(sig, frame):
    global pause_processing
    print(
        "\nProcessing paused. Enter 'n' for next block, 'c' to continue, 'b' to set new breakpoint, 'q' to quit.\n"
    )
    pause_processing = True


def call_bitcoin_cli_command(args, command, input_data=None):
    global total, success, failed
    total += 1
    cmd = [
        args.cli,
        f"-datadir={args.datadir}",
    ]
    cmd.extend(command)
    try:
        process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        stdout, stderr = process.communicate(input=input_data)

        if process.returncode != 0:
            print(f"Error executing command: {command}")
            print(stderr)
            failed += 1
            return None
        success += 1
        return stdout
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {command}")
        print(e.output)
        failed += 1
        return None


def process_log_file(args):
    global pause_processing
    stop_block_hash = args.stop

    def user_prompt():
        global pause_processing
        nonlocal stop_block_hash

        while pause_processing:
            command = input("(Enter command:) ").strip()
            if command == "n":
                break
            elif command == "c":
                pause_processing = False
                break
            elif command == "b":
                new_stop = input("Enter new stop block hash: ").strip()
                stop_block_hash = new_stop
            elif command == "q":
                print("Quitting.")
                sys.exit(0)
            else:
                print("Unknown command")

    # Step 1: Find the start point
    with open(args.logfile, "r") as file:
        start_position = None
        while True:
            pos = file.tell()
            line = file.readline()
            if not line:
                break
            parts = line.split()

            if len(parts) <= 2:
                print(f"Skipping line with unrecognised content: {line.strip()}")
                continue

            if parts[1] == "block":
                hash = parts[2]
                if (args.start is None) or (hash == args.start):
                    invalidate_command = [
                        "invalidateblock",
                        hash,
                    ]
                    print(f"Invalidating block: {hash}")
                    call_bitcoin_cli_command(args, invalidate_command)
                    start_position = pos

                    # Shutdown bitcoind
                    call_bitcoin_cli_command(args, ["stop"])
                    time.sleep(20)

                    # mv mempool.dat
                    subprocess.run(
                        [
                            "mv",
                            f"{args.datadir}/mempool.dat",
                            f"{args.datadir}/bak.mempool.dat",
                        ]
                    )  # doesn't capture output

                    # Start it back up
                    subprocess.run([args.daemon, f"-datadir={args.datadir}"])
                    time.sleep(45)
                    break

            if pause_processing:
                user_prompt()

    if not start_position:
        b_str = (
            f"block hash {args.start}"
            if args.start is not None
            else 'any "block" lines'
        )
        print(f"Error, could not find {b_str} in logfile {args.logfile}")
        sys.exit(1)

    # Step 2: Read line-by-line from start point
    submit_block_command = [
        "reconsiderblock",
        args.start,
        "1",
    ]
    print(f"Submitting block: {args.start=:}")
    call_bitcoin_cli_command(args, submit_block_command)

    with open(args.logfile, "r") as file:
        file.seek(start_position)
        for line in file:
            if pause_processing:
                user_prompt()
            parts = line.split()

            if len(parts) <= 2:
                print(f"Skipping line with unrecognised content: {line.strip()}")
                continue

            if parts[1] == "transaction":
                txid = parts[2]
                wtxid = parts[3]
                tx_hex = parts[4]
                send_transaction_command = [
                    "-stdin",
                    "sendrawtransaction",
                ]
                print(f"Resending transaction: {txid=:}, {wtxid=:}")
                call_bitcoin_cli_command(
                    args, send_transaction_command, input_data=tx_hex
                )
            elif parts[1] == "block":
                block_hash = parts[2]
                if (
                    stop_block_hash and block_hash == stop_block_hash
                ) or pause_processing:
                    print(f"Reached stop block: {stop_block_hash}")
                    user_prompt()

                submit_block_command = [
                    "reconsiderblock",
                    block_hash,
                    "1",
                ]
                print(f"Resubmitting block: {block_hash=:}")
                call_bitcoin_cli_command(args, submit_block_command)
            else:
                print(f"Skipping line with unrecognised content: {line}")


if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal_handler)

    cmd = input(
        "WARNING: this will erase mempool.dat. Are you sure you wish to continue?"
    ).strip()
    if cmd == "y" or cmd == "yes":
        pass
    else:
        print("Quitting.")
        sys.exit(0)

    parser = argparse.ArgumentParser(description="Process a replay log file.")
    parser.add_argument("logfile", type=str, help="Path to replay.log")
    parser.add_argument(
        "--start", type=str, help="Start block hash (will be invalidated)"
    )
    parser.add_argument("--stop", type=str, help="Stop block hash")
    parser.add_argument("--daemon", type=str, help="path to bitcoind binary")
    parser.add_argument("--cli", type=str, help="path to bitcoin-cli binary")
    parser.add_argument("--datadir", type=str, help="datadir of bitcoind to replay")

    args = parser.parse_args()

    process_log_file(args)

    print(f"Total calls: {total}")
    print(f"Successfully added: {success}")
    print(f"Failed to be added: {failed}")
