import pandas as pd
import re
import argparse

def extract_at_command_data(row):
    data = {}
    row_header = row[0].split(";")
    data['Timestamp'] = row_header[0].strip('"')
    data['Received Bytes'] = int(row_header[1].strip('"'))
    data['Transmitted Bytes'] = int(row_header[2].strip('"'))
    data['Delay'] = row_header[3].strip('"')

    qscan_cell_ids, qscan_frequencies, qscan_bandwidths = [], [], []
    qscan_rsrp, qscan_rsrq, qscan_srxlev, qscan_scs = [], [], [], []

    for entry in row[1:]:
        entry = entry.strip()
        if entry.startswith('+CSQ:'):
            match = re.search(r'\+CSQ: (\d+),(\d+)', entry)
            if match:
                data['Signal Quality'] = match.group(1)
                data['Bit Error Rate'] = match.group(2)
        elif entry.startswith('+CREG:'):
            match = re.search(r'\+CREG: (\d+),(\d+)', entry)
            if match:
                data['Registration Status'] = match.group(2)
        elif entry.startswith("+QNWINFO:"):
            match = re.search(r"\+QNWINFO: '(\w+)',\s*'(\d+)',\s*'([^']+)',(\d+)", entry)
            if match:
                data['Network Mode'] = match.group(1)
                data['Network Operator'] = match.group(2)
                data['Network Band'] = match.group(3)
                data['Cell ID'] = match.group(4)
        elif entry.startswith("+QENG: 'servingcell'"):
            serving_cell_data = entry.split(',')
            if len(serving_cell_data) >= 16:
                data['Serving Cell State'] = serving_cell_data[1]
                data['Duplex Mode'] = serving_cell_data[3]
                data['MCC'] = serving_cell_data[4]
                data['MNC'] = serving_cell_data[5]
                data['Cell ID'] = serving_cell_data[6]
                data['PCID'] = serving_cell_data[7]
                data['TAC'] = serving_cell_data[8]
                data['ARFCN'] = serving_cell_data[9]
                data['Band'] = serving_cell_data[10]
                data['NR_DL Bandwidth'] = serving_cell_data[11]
                data['RSRP'] = serving_cell_data[12]
                data['RSRQ'] = serving_cell_data[13]
                data['SINR'] = serving_cell_data[14]
                data['SCS'] = serving_cell_data[15]
                data['SRXLEV'] = serving_cell_data[16]
        elif entry.startswith("+QSCAN:"):
            qscan_entries = entry.split("+QSCAN:")
            for qscan_entry in qscan_entries:
                qscan_entry = qscan_entry.strip()
                if 'NR5G' in qscan_entry:
                    qscan_5g_data = qscan_entry.split(',')
                    if len(qscan_5g_data) >= 12:
                        #Checar position of the id later (talvez 8!)
                        qscan_cell_ids.append(qscan_5g_data[9].strip())
                        qscan_frequencies.append(qscan_5g_data[3].strip())
                        qscan_rsrp.append(qscan_5g_data[5].strip())
                        qscan_rsrq.append(qscan_5g_data[6].strip())
                        qscan_srxlev.append(qscan_5g_data[7].strip())
                        qscan_scs.append(qscan_5g_data[8].strip())
                        qscan_bandwidths.append(qscan_5g_data[11].strip())

    if qscan_cell_ids:
        #Talvez eu precise de aspas duplas
        data['QSCAN NR5G Cells Found (Count)'] = len(qscan_cell_ids)
        data['QSCAN NR5G Cell IDs'] = ', '.join(qscan_cell_ids)
        data['QSCAN NR5G Frequencies'] = ', '.join(qscan_frequencies)
        data['QSCAN NR5G RSRP'] = ', '.join(qscan_rsrp)
        data['QSCAN NR5G RSRQ'] = ', '.join(qscan_rsrq)
        data['QSCAN NR5G SRXLEV'] = ', '.join(qscan_srxlev)
        data['QSCAN NR5G SCS'] = ', '.join(qscan_scs)
        data['QSCAN NR5G Bandwidths'] = ', '.join(qscan_bandwidths)

    return data

def calculate_throughput(modem_data_df):
    modem_data_df['Timestamp'] = pd.to_datetime(modem_data_df['Timestamp'])
    modem_data_df['DL Th'] = 0.0
    modem_data_df['UL Th'] = 0.0

    for i in range(1, len(modem_data_df)):
        time_diff = (modem_data_df.loc[i, 'Timestamp'] - modem_data_df.loc[i - 1, 'Timestamp']).total_seconds()
        if time_diff > 0:
            received_diff = modem_data_df.loc[i, 'Received Bytes'] - modem_data_df.loc[i - 1, 'Received Bytes']
            transmitted_diff = modem_data_df.loc[i, 'Transmitted Bytes'] - modem_data_df.loc[i - 1, 'Transmitted Bytes']
            modem_data_df.loc[i, 'DL Th'] = received_diff / time_diff
            modem_data_df.loc[i, 'UL Th'] = transmitted_diff / time_diff

    return modem_data_df.drop(columns=['Received Bytes', 'Transmitted Bytes'])

def main(modem_data_path, position_log_path, output_file, inner_merge=False):
    with open(modem_data_path, 'r') as modem_file:
        modem_data_raw = modem_file.readlines()

    modem_data_parsed = []
    current_row = []

    for line in modem_data_raw:
        line = line.strip()
        if re.match(r'".*\d{2}:\d{2}:\d{2}".*', line):
            if current_row:
                modem_data_parsed.append(current_row)
            current_row = [line]
        else:
            current_row.append(line)

    if current_row:
        modem_data_parsed.append(current_row)

    modem_data_cleaned = [extract_at_command_data(row) for row in modem_data_parsed[1:]]
    modem_data_df = pd.DataFrame(modem_data_cleaned)
    modem_data_df = calculate_throughput(modem_data_df)

    position_log_df = pd.read_csv(position_log_path)
    position_log_df.columns = position_log_df.columns.str.strip()

    if 'z' in position_log_df.columns:
        position_log_df.drop(columns=['z'], inplace=True)

    modem_data_timestamp_col = next(col for col in modem_data_df.columns if col.lower() == 'timestamp')
    position_log_timestamp_col = next(col for col in position_log_df.columns if col.lower() == 'timestamp')

    modem_data_df[modem_data_timestamp_col] = pd.to_datetime(modem_data_df[modem_data_timestamp_col], errors='coerce')
    position_log_df[position_log_timestamp_col] = pd.to_datetime(position_log_df[position_log_timestamp_col], errors='coerce')

    modem_data_df.drop_duplicates(subset=[modem_data_timestamp_col], inplace=True)
    position_log_df.drop_duplicates(subset=[position_log_timestamp_col], inplace=True)

    merged_data = pd.merge(modem_data_df, position_log_df, left_on=modem_data_timestamp_col, right_on=position_log_timestamp_col, how='outer' if not inner_merge else 'inner')

    if position_log_timestamp_col in merged_data.columns:
        merged_data.drop(columns=[position_log_timestamp_col], inplace=True)

    cols = list(merged_data.columns)
    timestamp_index = cols.index('Timestamp')
    
    if 'x' in cols:
        cols.remove('x')
    if 'y' in cols:
        cols.remove('y')
    if 'DL Th' in cols:
        cols.remove('DL Th')
    if 'UL Th' in cols:
        cols.remove('UL Th')

    new_order = (
        cols[:timestamp_index + 1] +
        ['x', 'y', 'DL Th', 'UL Th'] +
        cols[timestamp_index + 1:]
    )
    
    merged_data = merged_data[new_order]
    merged_data.to_csv(output_file, index=False)

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('modem_data_file')
    parser.add_argument('position_log_file')
    parser.add_argument('output_file')
    parser.add_argument('--inner', action='store_true')
    
    args = parser.parse_args()
    main(args.modem_data_file, args.position_log_file, args.output_file, args.inner)
