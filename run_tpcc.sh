OUTPUT_DIR=$1
DB_EXECUTABLE=$2

GREP_PATTERN="Ready. Waiting for run command from the controller."
PORT=8989

NUM_WAREHOUSES=("1" "2" "4" "8" "16" "32" "64")

RED="\033[31m"
GREEN="\033[32m"
YELLOW="\033[33m"
BLUE="\033[34m"
MAGENTA="\033[35m"
CYAN="\033[36m"
GREY="\033[90m"
RESET="\033[0m"

# Function to handle SIGINT (Ctrl+C)
handle_sigint() {
  echo -e "${RED}Received Ctrl+C. Killing detached subprocesses $PID...${RESET}"
  kill $PID 2>/dev/null
  exit 1
}

# Set up the signal handler for SIGINT
trap handle_sigint SIGINT

mkdir -p $OUTPUT_DIR
START_TIME=$SECOND

for num_warehouses in "${NUM_WAREHOUSES[@]}"; do
  for repeat in {1..5}; do
    echo -e "Starting execution caracal_tpcc_${num_warehouses}_${repeat}"
    OUTPUT_FILENAME="caracal_tpcc_${num_warehouses}_${repeat}.txt"
    rm -f $OUTPUT_DIR/$OUTPUT_FILENAME

    split_threshold=3
    if [ $num_warehouses -gt 7 ]; then
      split_threshold=1000000
    fi

    # Run TPCC
    echo -e "${CYAN}$DB_EXECUTABLE -c 127.0.0.1:8989 -n host1 -w tpcc -XMaxNodeLimit1 -Xcpu32 -Xmem30G -XEpochSize100000 -XNrEpoch20 -XVHandleBatchAppend -XTpccWarehouses${num_warehouses} -XOnDemandSplitting${split_threshold}${RESET}"
    $DB_EXECUTABLE -c 127.0.0.1:8989 -n host1 -w tpcc -XMaxNodeLimit1 -Xcpu32 -Xmem30G -XEpochSize100000 -XNrEpoch20 -XVHandleBatchAppend "-XTpccWarehouses${num_warehouses}" "-XOnDemandSplitting${split_threshold}" >$OUTPUT_DIR/$OUTPUT_FILENAME &

    PID=$!
    echo -e "${GREY}Benchmark started with pid: $PID${RESET}"

    # Wait for initialization to finish and start the benchmark
    while true; do
      result=$(tail -n 1 "$OUTPUT_DIR/$OUTPUT_FILENAME" 2>/dev/null | grep --line-buffered -m 1 -E "$GREP_PATTERN")

      if [ ! -z "$result" ]; then
        echo -e "${GREY}Initialization ready, start${RESET}"
        curl -X POST -H "Content-Type: application/json" -d "{\"type\": \"status_change\", \"status\": \"connecting\"}" http://127.0.0.1:7878/broadcast/ >/dev/null 2>&1
        break
      fi

      if kill -0 $PID 2>/dev/null; then
        sleep 1
      else
        echo -e "${RED}Benchmark process $PID exited unexpectedly.${RESET}"
        break
      fi
    done

    wait $PID

    echo -e "${GREY}Waiting for port $PORT to be free...${RESET}"
    # wait for the port to be free
    while true; do
      # Check if the port is in use using ss
      ss -n | grep -q ":$PORT"

      # If the exit status of grep is non-zero, the port is not in use
      if [ $? -ne 0 ]; then
        echo -e "${GREY}Port $PORT is now free.${RESET}"
        break
      fi

      # Wait for a short interval before checking again
      sleep 1
    done
    echo -e "${GREEN}Execution finished${RESET}"
  done
  ELAPSED_TIME=$((SECONDS - START_TIME))
  echo -e "${BLUE}Current Runtime ${ELAPSED_TIME} s${RESET}"
done
