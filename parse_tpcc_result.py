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

THROUGHPUT_PATTERN = re.compile(r"Throughput (\d+) txn/s")

num_warehouses = ["1", "2", "4", "8", "16", "32", "64"]
repeat = 3


def main(logs_dir, output_dir):
    logging.info(f"Parsing logs from directory {logs_dir} and saving to {output_dir}")
    if not os.path.exists(output_dir):
        logging.info("Creating output directory")
        os.makedirs(output_dir)
    logging.info(f"num_warehouses: {num_warehouses}")
    logging.info(f"repeat: {repeat}")

    table = [['num_warehouses', 'throughput']]
    for num_w in num_warehouses:
        throughput = []
        for i in range(1, repeat + 1):
            filename = f"caracal_tpcc_{num_w}_{i}.txt"
            filepath = os.path.join(logs_dir, filename)
            logging.info(f"parsing {filename}")
            with open(filepath, 'r') as f:
                lines = f.readlines()
                logging.debug(f"found {len(lines)} lines")
                for line in lines:
                    match = THROUGHPUT_PATTERN.search(line)
                    if match:
                        throughput.append(float(match.group(1)) / 1e6)
                        break
        logging.debug(f"found {len(throughput)} throughput values: {throughput}")
        row = [num_w, np.mean(throughput)]
        table.append(row)
    logging.info(f"saving table for caracal_tpcc")
    csv_filename = f"caracal_tpcc.csv"
    with open(os.path.join(output_dir, csv_filename), 'w') as f:
        writer = csv.writer(f)
        writer.writerows(table)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python script.py <directory_path> <output_path>")
        sys.exit(1)

    main(sys.argv[1], sys.argv[2])
