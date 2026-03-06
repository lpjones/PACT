#!/bin/bash
# Usage:
#   ./node_mem_toggle.sh offline 10
#   ./node_mem_toggle.sh online 10
#   ./node_mem_toggle.sh offline all

NODE="node0"
ACTION="$1"
COUNT="$2"

if [[ "$ACTION" != "online" && "$ACTION" != "offline" ]]; then
    echo "Usage: $0 {online|offline} {N|all}"
    exit 1
fi

if [[ -z "${COUNT:-}" ]]; then
    echo "Usage: $0 {online|offline} {N|all}"
    exit 1
fi

NODE_PATH="/sys/devices/system/node/${NODE}"

if [[ ! -d "$NODE_PATH" ]]; then
    echo "Error: $NODE_PATH does not exist"
    exit 1
fi

processed=0

# Drop page cache first
sudo sync
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

for memdir in "$NODE_PATH"/memory*; do
    ONLINE_FILE="$memdir/online"
    REMOVABLE_FILE="$memdir/removable"

    if [[ -f "$ONLINE_FILE" ]]; then

        if [[ "$COUNT" != "all" && "$processed" -ge "$COUNT" ]]; then
            break
        fi

        if [[ "$ACTION" == "offline" ]]; then

            # Check if removable
            if [[ -f "$REMOVABLE_FILE" ]]; then
                removable=$(cat "$REMOVABLE_FILE")
                if [[ "$removable" == "0" ]]; then
                    echo "Skipping $(basename "$memdir") (not removable)"
                    continue
                fi
            fi

            echo "Offlining $(basename "$memdir")"

            # Try up to 5 times
            for attempt in {1..5}; do
                echo 1 | sudo tee /proc/sys/vm/compact_memory > /dev/null

                if echo 0 | sudo tee "$ONLINE_FILE" > /dev/null 2>&1; then
                    echo "  Success on attempt $attempt"
                    processed=$((processed + 1))
                    break
                else
                    echo "  Attempt $attempt failed"
                fi
            done

        else
            echo "Onlining $(basename "$memdir")"
            echo 1 | sudo tee "$ONLINE_FILE" > /dev/null
            processed=$((processed + 1))
        fi
    fi
done

echo "Done. Processed $processed memory blocks."