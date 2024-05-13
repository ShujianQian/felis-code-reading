import re
import os
import glob
import sys
import csv
import logging
import numpy as np

logging.basicConfig(level=logging.INFO, format='[%(asctime)s] [%(levelname)s] - %(message)s')
root = logging.getLogger()
handler = logging.FileHandler('parse_experiment.log', 'w+')
handler.setFormatter(logging.Formatter('[%(asctime)s] [%(levelname)s] - %(message)s'))
root.addHandler(handler)

THROUGHPUT_PATTERN = re.compile(r"Throughput (\d+) txn/s")

alphas = ["0", "20", "40", "60", "80", "90", "95", "99"]
workloads = ["ycsba", "ycsbb", "ycsbc", "ycsbf"]
read_types = ["full"]
repeat = 3

def main(logs_dir, output_dir):
    logging.info(f"Parsing logs from directory {logs_dir} and saving to {output_dir}")
    if not os.path.exists(output_dir):
        logging.info("Creating output directory")
        os.makedirs(output_dir)
    logging.info(f"alphas: {alphas}")
    logging.info(f"workloads: {workloads}")
    logging.info(f"read_types: {read_types}")
    logging.info(f"repeat: {repeat}")

    for workload in workloads:
        for read_type in read_types:
            table = [['alpha', 'throughput']]
            for alpha in alphas:
                throughput = []
                for i in range(1, repeat + 1):
                    filename = f"caracal_{read_type}_{workload}_{alpha}_{i}.txt"
                    filepath = os.path.join(logs_dir, filename)
                    logging.info(f"Parsing {filename}")
                    with open(filepath, 'r') as f:
                        lines = f.readlines()
                        logging.debug(f"Found {len(lines)} lines")
                        for line in lines:
                            match = THROUGHPUT_PATTERN.search(line)
                            if match:
                                throughput.append(float(match.group(1)) / 1e6)
                                break
                logging.debug(f"Found {len(throughput)} throughput values: {throughput}")
                row = [float(alpha), np.mean(throughput)]
                table.append(row)
            logging.info(f"Saving table for caracal_{read_type}_{workload}")
            csv_filename = f"caracal_{workload}_{read_type}.csv"
            with open(os.path.join(output_dir, csv_filename), 'w') as f:
                writer = csv.writer(f)
                writer.writerows(table)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python script.py <directory_path> <output_path>")
        sys.exit(1)

    main(sys.argv[1], sys.argv[2])