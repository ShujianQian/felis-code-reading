import csv
import logging
import os
import re
import sys

import numpy as np

logging.basicConfig(level=logging.INFO, format='[%(asctime)s] [%(levelname)s] - %(message)s')
root = logging.getLogger()
handler = logging.FileHandler('parse_experiment.log', 'w+')
handler.setFormatter(logging.Formatter('[%(asctime)s] [%(levelname)s] - %(message)s'))
root.addHandler(handler)

THROUGHPUT_PATTERN = re.compile(r"Throughput (\d+) txn\/s")
LATENCY_PATTERN = re.compile(r"Insert / Initialize / Execute (\d+) ms (\d+) ms (\d+) ms")

alphas = ["0", "99"]
epoch_sizes = [5000, 6000, 7000, 8000, 9000, 10000, 12000, 14000, 16000, 18000, 20000, 25000, 30000, 35000, 40000, 45000, 50000]
repeat = 5


def main(logs_dir, output_dir):
    logging.info(f"Parsing logs from directory {logs_dir} and saving to {output_dir}")
    if not os.path.exists(output_dir):
        logging.info("Creating output directory")
        os.makedirs(output_dir)
    logging.info(f"epoch_sizes: {epoch_sizes}")
    logging.info(f"repeat: {repeat}")

    for alpha in alphas:
        table = [['epoch_size', 'throughput', 'latency']]
        for epoch_size in epoch_sizes:
            throughput = []
            latency = []
            for i in range(1, repeat + 1):
                filename = f"caracal_full_ycsbf_{alpha}_{epoch_size}_{i}.txt"
                filepath = os.path.join(logs_dir, filename)
                logging.info(f"parsing {filename}")
                with open(filepath, 'r') as f:
                    lines = f.readlines()
                    logging.debug(f"found {len(lines)} lines")
                    for line in lines:
                        match = THROUGHPUT_PATTERN.search(line)
                        if match:
                            throughput.append(float(match.group(1)) / 1e6)
                        match = LATENCY_PATTERN.search(line)
                        if match:
                            latency.append((float(match.group(1)) + float(match.group(2)) + float(match.group(3))) / 19)
            logging.debug(f"found {len(throughput)} throughput values: {throughput}")
            logging.debug(f"found {len(latency)} latency values: {latency}")
            row = [epoch_size, np.mean(throughput), np.mean(latency)]
            table.append(row)
        logging.info(f"saving table for caracal_tpcc")
        csv_filename = f"caracal_ycsbf_{alpha}_latency.csv"
        with open(os.path.join(output_dir, csv_filename), 'w') as f:
            writer = csv.writer(f)
            writer.writerows(table)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python script.py <directory_path> <output_path>")
        sys.exit(1)

    main(sys.argv[1], sys.argv[2])
