/**  RM500Q Modem Monitor
 *      
 *   This is program to gather data from a Quectel RM500Q-Gl modem. The read parameters can be passed 
 * via a configuration file or informed when running the application. The data is requested to the modem
 * via AT commands and are printed on the screen and stored in a csv file.   
 *      
 * 
 *   @author Manoel Narciso Reis Soares Filho    
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>

#define DEFAULT_DEVICE "/dev/ttyUSB3"
#define DEFAULT_BAUD_RATE 115200
#define DEFAULT_INTERVAL 1000 
#define DEFAULT_OUTPUT_FOLDER "."
#define DEFAULT_INTERFACE_NAME "wlp0s20f3" 
#define MAX_OUTPUT_PING 1024
#define READING_ERROR_LABEL "MODEM_READING_ERROR\n"
#define DEFAULT_PING_IP "10.0.3.1"
#define DEFAULT_BLOCKED_RESOURCE_TOLERANCE 5000

typedef struct {
    unsigned long total_bytes_rx;
    unsigned long total_bytes_tx;
} DataUsage;

volatile double last_delay = 0.0;
volatile sig_atomic_t running = 1;
int modem_number = -1;
int reading_failing_tolerance = DEFAULT_BLOCKED_RESOURCE_TOLERANCE;

int configure_serial_port(int fd, int baud_rate);
int send_at_command(int fd, const char *command);
void flush_serial_port(int fd);
int read_response(int fd, char *response, size_t max_len);
int request_modem_property(int fd, const char *command, char *response, size_t max_len);
void process_commands(int fd, char *commands[], int count, FILE *csv_file, long rx_bytes, long tx_bytes );
int read_config_file(const char *filename, char **device, int *baud_rate, char *commands[], int max_count, int *interval, char **output_folder, char **interface_name, char **ping_ip);
void to_lowercase(char *str);
void trim_whitespace(char **str);
void remove_surrounding_quotes(char *str);
void signal_handler(int signum);
FILE *create_csv_file(char *commands[], int count, const char *output_folder);
void get_network_statistics(const char *interface, long *rx_bytes, long *tx_bytes);
void calculate_throughput(const char *interface, int interval_mseconds, double *rx_throughput, double *tx_throughput);
long read_bytes(const char *path);
void *ping_address(void *arg);
void get_modem_number();
char *run_command(const char *command);
DataUsage extract_data_usage(volatile char *bearer_output_response);
volatile DataUsage get_data_usage();

int main(int argc, char *argv[]) {
    char *device = NULL;
    int baud_rate = DEFAULT_BAUD_RATE;
    int interval = DEFAULT_INTERVAL;
    char *output_folder = DEFAULT_OUTPUT_FOLDER;
    int command_count = 0;
    char *commands[100];
    char *interface_name = strdup(DEFAULT_INTERFACE_NAME);
    char *ping_ip = strdup(DEFAULT_PING_IP);

    device = strdup(DEFAULT_DEVICE);

    int file_mode = 0;
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 < argc) {
                file_mode = 1;
                filename = argv[++i];
            } else {
                fprintf(stderr, "Error: -c flag requires a filename.\n");
                return 1;
            }
        } else {
            commands[command_count++] = argv[i];
        }
    }

    if (file_mode) {
        int count = read_config_file(filename, &device, &baud_rate, commands, sizeof(commands) / sizeof(commands[0]), &interval, &output_folder, &interface_name, &ping_ip);
        if (count < 0) {
            fprintf(stderr, "Error reading configuration from file '%s'\n", filename);
            free(device);
            return 1;
        }
        command_count = count;
    }

    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror("open");
        free(device);
        return 1;
    }

    if (configure_serial_port(fd, baud_rate) != 0) {
        close(fd);
        free(device);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    FILE *csv_file = create_csv_file(commands, command_count, output_folder);
    if (csv_file == NULL) {
        close(fd);
        free(device);
        return 1;
    }


    pthread_t ping_thread;
    if (pthread_create(&ping_thread, NULL, ping_address, ping_ip) != 0) {
        perror("Failed to create thread");
        return 1;
    }

    get_modem_number();
    if (modem_number == -1) {
        printf("Modem number not found. Exiting.\n");
        return EXIT_FAILURE;
    }

    long rx_bytes, tx_bytes;
    DataUsage data_usage = {0, 0};
    while (running) {
        data_usage = get_data_usage();
        rx_bytes = data_usage.total_bytes_rx;
        tx_bytes = data_usage.total_bytes_tx;
        if (!running) break;
        process_commands(fd, commands, command_count, csv_file, rx_bytes, tx_bytes);
        usleep(interval * 1000); 
    }

    fclose(csv_file);
    close(fd);

    free(device);
    free(interface_name);
    if (file_mode) {
        for (int i = 0; i < command_count; i++) {
            free(commands[i]);
        }
        free(output_folder);
    }

    pthread_join(ping_thread, NULL);
    fprintf(stderr, "Program terminated properly.\n");
    return 0;
}

int configure_serial_port(int fd, int baud_rate) {
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfsetospeed(&tty, baud_rate);
    cfsetispeed(&tty, baud_rate);

    tty.c_cflag &= ~PARENB; // No parity bit
    tty.c_cflag &= ~CSTOPB; // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8; // 8 data bits

    // No flow control
    tty.c_cflag &= ~CRTSCTS;

    // Enable the receiver and set local mode
    tty.c_cflag |= (CLOCAL | CREAD);

    // Disable canonical mode, echo, and signals
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // Disable output processing
    tty.c_oflag &= ~OPOST;

    // Set read timeout
    tty.c_cc[VMIN] = 0;    // Non-blocking read
    tty.c_cc[VTIME] = 10;  // 1 second timeout (10 deciseconds)

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

int send_at_command(int fd, const char *command) {
    char cmd_with_cr[256];
    snprintf(cmd_with_cr, sizeof(cmd_with_cr), "%s\r", command);
    ssize_t n = write(fd, cmd_with_cr, strlen(cmd_with_cr));
    
    if (n < 0) {
        perror("write");
        return -1;
    }

    return 0;
}

void flush_serial_port(int fd) {
    tcflush(fd, TCIOFLUSH);
}

int read_response(int fd, char *response, size_t max_len) {
    size_t total_read = 0;
    int bytes_read;
    int max_blocks = DEFAULT_BLOCKED_RESOURCE_TOLERANCE;

    while (total_read < max_len - 1) {
        bytes_read = read(fd, response + total_read, max_len - total_read - 1);
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
            	max_blocks--;
            	if(!max_blocks){
            		strcpy(response, READING_ERROR_LABEL);
            		total_read = strlen(READING_ERROR_LABEL);
            		flush_serial_port(fd);
            		break;
            	}
                continue;
            }
            perror("read");
            return -1;
        } else if (bytes_read == 0) {
            break;
        }
        total_read += bytes_read;

        if (strchr(response, '\n') != NULL) {
            break;
        }
    }

    response[total_read] = '\0';

    for(int i = 0; i < strlen(response); i++){
        if(response[i] == '\"')
            response[i] = '\'';
    } 
    return total_read;
}

int request_modem_property(int fd, const char *command, char *response, size_t max_len) {
    if (send_at_command(fd, command) != 0) {
        return -1;
    }

    if (read_response(fd, response, max_len) < 0) {
        return -1;
    }

    // if(strncmp(command, "AT+QPING", 8) == 0){
    //     char response_ping[1024];
    //     int ping_amount = 0;

    //     for(int i = 0 ; i < ping_amount ; i++){
    //         if(read_response(fd, response_ping, max_len) < 0){
    //             return -1;
    //         }   
    //         strcat(response,response_ping);
    //     }
    // }

    // if(strcmp(command,"    AT+QPING=1,\"www.google.com\",1,1") == 0){
    //     char response_ping[1024];
        
    //     if(read_response(fd, response_ping, max_len) < 0){
    //         return -1;
    //     }
    //     strcat(response,response_ping);
    // }

    return 0;
}

void process_commands(int fd, char *commands[], int count, FILE *csv_file, long rx_bytes, long tx_bytes ) {
    char response[1024];
    char *responses[count];

    for (int i = 0; i < count; i++) {
        responses[i] = malloc(1024); 
        if (responses[i] == NULL) {
            perror("Error allocating memory for response");
            return;
        }
    }

    for (int i = 0; i < count; i++) {
        const char *at_command = commands[i];

        flush_serial_port(fd);
        if (request_modem_property(fd, at_command, responses[i], sizeof(response)) != 0) {
            fprintf(stderr, "Error processing command '%s'\n", at_command);
            strcpy(responses[i], "ERROR");
        }
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[256];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    printf("Timestamp: %s\n", timestamp);
    fprintf(csv_file, "\"%s\"", timestamp);
    fprintf(csv_file, ";\"%ld\"", rx_bytes);
    fprintf(csv_file, ";\"%ld\"", tx_bytes);
    fprintf(csv_file, ";\"%lf\"", last_delay);

    for (int i = 0; i < count; i++) {
        printf("Command: %s\nResponse: %s\n\n", commands[i], responses[i]);
        fprintf(csv_file, ";\"%s\"", responses[i]);
        free(responses[i]);
    }

    fprintf(csv_file, "\n");
}

int read_config_file(const char *filename, char **device, int *baud_rate, char *commands[], int max_count, int *interval, char **output_folder, char **interface_name, char **ping_ip) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error opening configuration file");
        fprintf(stderr, "File path provided: %s\n", filename); // Additional debug info
        return -1;
    }

    char line[256];
    int count = 0;
    int in_commands_block = 0;
    char command_buffer[1024] = {0}; // Buffer to accumulate commands across lines

    *output_folder = strdup(DEFAULT_OUTPUT_FOLDER);
    if (*output_folder == NULL) {
        perror("Error allocating memory for output folder");
        fclose(file);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        char lower_line[256];
        strncpy(lower_line, line, sizeof(lower_line));
        lower_line[sizeof(lower_line) - 1] = '\0';
        to_lowercase(lower_line);

        if (strncmp(lower_line, "device:", 7) == 0) {
            free(*device);
            *device = strdup(line + 7);
            if (*device == NULL) {
                perror("Error allocating memory for device");
                fclose(file);
                return -1;
            }
            trim_whitespace(device);
            remove_surrounding_quotes(*device);
        } else if (strncmp(lower_line, "baud_rate:", 10) == 0) {
            *baud_rate = atoi(line + 10);
        } else if (strncmp(lower_line, "commands:", 9) == 0) {
            in_commands_block = 1; 
            continue;
        } else if (strncmp(lower_line, "interval:", 9) == 0) {
            *interval = atoi(line + 9);
        } else if (strncmp(lower_line, "ping_ip:", 8) == 0) {
            free(*ping_ip);
            *ping_ip = strdup(line + 8);
        } else if (strncmp(lower_line, "output_folder:", 14) == 0) {
            free(*output_folder);
            *output_folder = strdup(line + 14);
            if (*output_folder == NULL) {
                perror("Error allocating memory for output folder");
                fclose(file);
                return -1;
            }
            trim_whitespace(output_folder);
            remove_surrounding_quotes(*output_folder);
        }else if (strncmp(lower_line, "interface_name:", 15) == 0) {
            free(*interface_name);
            *interface_name = strdup(line + 15);
            if (*interface_name == NULL) {
                perror("Error allocating memory for interface name");
                fclose(file);
                return -1;
            }
            trim_whitespace(interface_name);
            remove_surrounding_quotes(*interface_name);
        }else if (strncmp(lower_line, "qscan_mode:", 11) == 0) {
            if(strcmp(strdup(line + 11), "active")){
                reading_failing_tolerance = 99999999;
            }
        }


        if (in_commands_block) {
            if (line[0] == '}') {
                break; 
            } else if (line[0] == '{') {
                continue; 
            }

            strcat(command_buffer, line);

            char *cmd = strtok(command_buffer, ";");
            while (cmd != NULL) {
                trim_whitespace(&cmd);
                remove_surrounding_quotes(cmd);

                commands[count] = strdup(cmd);
                if (commands[count] == NULL) {
                    perror("Error allocating memory for command");
                    fclose(file);
                    return -1;
                }
                count++;
                cmd = strtok(NULL, ";");

                if (count >= max_count) {
                    break; 
                }
            }

            memset(command_buffer, 0, sizeof(command_buffer));
        }
    }

    fclose(file);
    return count;
}

void to_lowercase(char *str) {
    for (char *p = str; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
}

void trim_whitespace(char **str) {
    char *end;

    while (isspace((unsigned char)**str)) (*str)++;

    if (**str == 0) 
        return;

    end = *str + strlen(*str) - 1;
    while (end > *str && isspace((unsigned char)*end)) end--;

    *(end + 1) = '\0';
}

void remove_surrounding_quotes(char *str) {
    size_t len = strlen(str);
    if (len > 1 && str[0] == '\"' && str[len - 1] == '\"') {
        memmove(str, str + 1, len - 1);
        str[len - 2] = '\0';
    }
}

FILE *create_csv_file(char *commands[], int count, const char *output_folder) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    struct stat st = {0};
    if (stat(output_folder, &st) == -1) {
        if (mkdir(output_folder, 0700) != 0) {
            perror("Error creating output folder");
            return NULL;
        }
    }

    char filename[256];
    snprintf(filename, sizeof(filename), "%s/modem_data_%04d-%02d-%02d_%02d-%02d-%02d.csv",
             output_folder,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        perror("Error creating CSV file");
        return NULL;
    }

    fprintf(file, "\"Timestamp\"");
    fprintf(file, ";\"Received Bytes\"");
    fprintf(file, ";\"Transmited Bytes\"");
    fprintf(file, ";\"Delay\"");
for (int i = 0; i < count; i++) {
    char *command = commands[i];
    
    if (strchr(command, '\"') != NULL) {
        fprintf(file, ";'");
        
        for (char *c = command; *c != '\0'; c++) {
            if (*c == '\"') {
                fputc('\'', file);
            } else {
                fputc(*c, file);
            }
        }

        fprintf(file, "'");
    } else {
        fprintf(file, ";\"%s\"", command);
    }
}
    fprintf(file, "\n");
    return file;
}

void signal_handler(int signum) {
    running = 0;
}

void calculate_throughput(const char *interface, int interval_mseconds, double *rx_throughput, double *tx_throughput) {
    long rx_bytes_start, tx_bytes_start;
    long rx_bytes_end, tx_bytes_end;

    get_network_statistics(interface, &rx_bytes_start, &tx_bytes_start);    
    if (!running) return;
    usleep(interval_mseconds*1000);
    get_network_statistics(interface, &rx_bytes_end, &tx_bytes_end);

    long rx_bytes_diff = rx_bytes_end - rx_bytes_start;
    long tx_bytes_diff = tx_bytes_end - tx_bytes_start;

    *rx_throughput = 1000 * (double)rx_bytes_diff / interval_mseconds;
    *tx_throughput = 1000 * (double)tx_bytes_diff / interval_mseconds;


    printf("Received Throughput: %.2f bytes/s (%.2f KB/s)\n", *rx_throughput, *rx_throughput / 1024);
    printf("Transmitted Throughput: %.2f bytes/s (%.2f KB/s)\n", *tx_throughput, *tx_throughput / 1024);
}

void get_network_statistics(const char *interface, long *rx_bytes, long *tx_bytes) {
    char rx_path[256], tx_path[256];

    snprintf(rx_path, sizeof(rx_path), "/sys/class/net/%s/statistics/rx_bytes", interface);
    snprintf(tx_path, sizeof(tx_path), "/sys/class/net/%s/statistics/tx_bytes", interface);

    *rx_bytes = read_bytes(rx_path);
    *tx_bytes = read_bytes(tx_path);
}

void get_network_statistics2(const char *interface, long *rx_bytes, long *tx_bytes) {
    char rx_path[256], tx_path[256];

    snprintf(rx_path, sizeof(rx_path), "/sys/class/net/%s/statistics/rx_bytes", interface);
    snprintf(tx_path, sizeof(tx_path), "/sys/class/net/%s/statistics/tx_bytes", interface);

    *rx_bytes = read_bytes(rx_path);
    *tx_bytes = read_bytes(tx_path);
}

long read_bytes(const char *path) {
    FILE *file = fopen(path, "r");
    long value = 0;

    if (file) {
        fscanf(file, "%ld", &value);
        fclose(file);
    } else {
        perror("Failed to open file");
    }

    return value;
}

void *ping_address(void *arg) {
    char *address = (char *)arg;
    char command[256];
    snprintf(command, sizeof(command), "ping -c 1 %s", address);

    FILE *fp;
    char output[MAX_OUTPUT_PING];

    while (running) {
        if ((fp = popen(command, "r")) == NULL) {
            perror("popen failed");
            return NULL;
        }

        while (fgets(output, sizeof(output), fp) != NULL) {
            if (strstr(output, "time=") != NULL) {
                char *time_start = strstr(output, "time=") + 5; 
                char *time_end = strstr(time_start, " ms"); 
                if (time_end != NULL) {
                    *time_end = '\0'; 
                    last_delay = atof(time_start);
                    printf("\n>> Delay to %s: %lf\n", (char *)arg, last_delay);
                }
            }
        }

        if (pclose(fp) == -1) {
            perror("pclose failed");
            return NULL;
        }

        sleep(1);
    }

    return NULL;
}

char *run_command(const char *command) {
    FILE *fp;
    char *output = malloc(4096);
    if (!output) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    fp = popen(command, "r");
    if (fp == NULL) {
        perror("popen");
        exit(EXIT_FAILURE);
    }

    size_t bytes_read = fread(output, 1, 4096, fp);
    output[bytes_read] = '\0';

    pclose(fp);
    return output;
}

void get_modem_number() {
    char *output = run_command("mmcli -L");
    char *modem_str = strstr(output, "Modem");
    if (modem_str) {
        sscanf(modem_str, "Modem/%d", &modem_number);
        printf(">>%s: \n", modem_str);
        printf("Found modem number: %d\n", modem_number);
    } else {
        printf("No modem found.\n");
    }
	
    modem_number = 0;
    free(output);
}

DataUsage extract_data_usage(volatile char *bearer_output_response) {
    const char *bearer_output = (const char*)bearer_output_response;
    DataUsage data_usage = {0, 0};
    char *rx_str = strstr(bearer_output, "total-bytes rx:");
    char *tx_str = strstr(bearer_output, "total-bytes tx:");

    if (rx_str && tx_str) {
        sscanf(rx_str, "total-bytes rx: %lu", &data_usage.total_bytes_rx);
        sscanf(tx_str, "total-bytes tx: %lu", &data_usage.total_bytes_tx);

        printf("Total bytes received: %lu\n", data_usage.total_bytes_rx);
        printf("Total bytes transmitted: %lu\n", data_usage.total_bytes_tx);
    } else {
        printf("Could not extract data usage.\n");
    }

    return data_usage; 
}

volatile DataUsage get_data_usage() {
    volatile char command[64];
    snprintf((char *)command, sizeof(command), "mmcli -b %d", modem_number);
    volatile char *bearer_output = run_command((const char *)command);

    DataUsage usage = extract_data_usage(bearer_output);    
    
    free((void*)bearer_output);
}


