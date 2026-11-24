#!/usr/bin/env bash
# Checks of online play on this machine, from the tree's root:
#   host/net/online-test.sh parse      STUN and signalling-record parsing
#   host/net/online-test.sh reject     wrong rooms and bad datagrams are refused
#   host/net/online-test.sh loopback   two processes, direct address, 25% datagram loss
#   host/net/online-test.sh room       two processes that know only the room code, through ntfy.sh
#   host/net/online-test.sh game       two Linux resonance_host processes (FREQ_SELFTEST=online)
#                                      pass the game's own packets (needs build/host/resonance_host)
# Each prints ONLINE_<MODE>_OK when every check holds.
set -euo pipefail
mode=${1:?mode}
tree=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf -- "$work"' EXIT
test=$work/online-test
g++ -std=gnu++17 -O1 -g -o "$test" "$tree/host/net/online-test.cpp" -lcurl -lcrypto -lpthread

# Two processes in a room of a fresh random code; the host on a free port. Arguments: the command
# of each side and the settings both share.
pair() {
    local command=$1
    shift
    local port=$((20000 + RANDOM % 20000))
    local room
    room=test-$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')
    env "$@" FREQ_ONLINE_ROOM="$room" FREQ_ONLINE_HOST=1 FREQ_ONLINE_PORT=$port \
        $command > "$work/host.log" 2>&1 &
    local host=$!
    env "$@" FREQ_ONLINE_ROOM="$room" $command > "$work/guest.log" 2>&1 &
    local guest=$!
    local status=0
    wait $host || status=1
    wait $guest || status=1
    sed 's/^/host:  /' "$work/host.log"
    sed 's/^/guest: /' "$work/guest.log"
    return $status
}

case $mode in
    parse | reject)
        "$test" "$mode" ;;
    loopback)
        # The guest's settings name the host: FREQ_ONLINE_PEER is filled in from the port below.
        port=$((20000 + RANDOM % 20000))
        room=test-$RANDOM$RANDOM
        common=(FREQ_ONLINE_ROOM=$room FREQ_ONLINE_SIGNAL=off FREQ_ONLINE_STUN=off
                FREQ_ONLINE_TEST_LOSS=25)
        env "${common[@]}" FREQ_ONLINE_HOST=1 FREQ_ONLINE_PORT=$port \
            "$test" side 300 > "$work/host.log" 2>&1 &
        host=$!
        env "${common[@]}" FREQ_ONLINE_PEER=127.0.0.1:$port \
            "$test" side 300 > "$work/guest.log" 2>&1 &
        guest=$!
        status=0
        wait $host || status=1
        wait $guest || status=1
        sed 's/^/host:  /' "$work/host.log"
        sed 's/^/guest: /' "$work/guest.log"
        [[ $status == 0 ]] && grep -q 'side 0: sent 300, received 300 in order' "$work/host.log" &&
            grep -q 'side 1: sent 300, received 300 in order' "$work/guest.log" &&
            echo ONLINE_LOOPBACK_OK ;;
    room)
        # Only the room code is shared: a side that reads the other's record on the ntfy topic
        # sends to its addresses, which is enough for both to link.
        pair "$test side 20" FREQ_ONLINE_STUN=off &&
            grep -q "found the \(guest\|host\)'s record" "$work/host.log" "$work/guest.log" &&
            grep -q 'side 0: sent 20, received 20 in order' "$work/host.log" &&
            grep -q 'side 1: sent 20, received 20 in order' "$work/guest.log" &&
            echo ONLINE_ROOM_OK ;;
    game)
        binary=$tree/build/host/resonance_host
        port=$((20000 + RANDOM % 20000))
        room=test-$RANDOM$RANDOM
        common=(FREQ_SELFTEST=online FREQ_ONLINE_ROOM=$room FREQ_ONLINE_SIGNAL=off
                FREQ_ONLINE_STUN=off)
        env "${common[@]}" FREQ_ONLINE_HOST=1 FREQ_ONLINE_PORT=$port \
            "$binary" > "$work/host.log" 2>&1 &
        host=$!
        env "${common[@]}" FREQ_ONLINE_PEER=127.0.0.1:$port "$binary" > "$work/guest.log" 2>&1 &
        guest=$!
        status=0
        wait $host || status=1
        wait $guest || status=1
        sed 's/^/host:  /' "$work/host.log"
        sed 's/^/guest: /' "$work/guest.log"
        [[ $status == 0 ]] && grep -q 'ONLINE_SELFTEST_SENT' "$work/host.log" &&
            grep -q 'ONLINE_SELFTEST_RECEIVED' "$work/guest.log" && echo ONLINE_GAME_OK ;;
    *)
        echo "online-test.sh: unknown mode $mode" >&2
        exit 2 ;;
esac
