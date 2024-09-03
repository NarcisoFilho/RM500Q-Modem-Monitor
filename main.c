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

// Default values
#define DEFAULT_DEVICE "/dev/ttyUSB3"
#define DEFAULT_BAUD_RATE 115200
#define DEFAULT_INTERVAL 1000 // Default interval in milliseconds
#define DEFAULT_OUTPUT_FOLDER "." // Default output folder is the current directory

// Global variable to handle termination
volatile sig_atomic_t running = 1;

// Function prototypes
int configure_serial_port(int fd, int baud_rate);
int send_at_command(int fd, const char *command);
void flush_serial_port(int fd);
int read_response(int fd, char *response, size_t max_len);
int request_modem_property(int fd, const char *command, char *response, size_t max_len);
void process_commands(int fd, char *commands[], int count, FILE *csv_file);
int read_config_file(const char *filename, char **device, int *baud_rate, char *commands[], int max_count, int *interval, char **output_folder);
void to_lowercase(char *str);
void trim_whitespace(char *str);
void remove_surrounding_quotes(char *str);
void signal_handler(int signum);
FILE *create_csv_file(char *commands[], int count, const char *output_folder);

// Main function
int main(int argc, char *argv[]) {
    char *device = NULL;
    int baud_rate = DEFAULT_BAUD_RATE; // Default baud rate
    int interval = DEFAULT_INTERVAL;   // Default interval in milliseconds
    char *output_folder = DEFAULT_OUTPUT_FOLDER; // Default output folder
    int command_count = 0;
    char *commands[100]; // Adjust the size as needed

    // Initialize default device if not provided
    device = strdup(DEFAULT_DEVICE);

    // Check for the -c flag
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
        // Read configuration from the file
        int count = read_config_file(filename, &device, &baud_rate, commands, sizeof(commands) / sizeof(commands[0]), &interval, &output_folder);
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

    // Configure the serial port
    if (configure_serial_port(fd, baud_rate) != 0) {
        close(fd);
        free(device);
        return 1;
    }

    // Set up signal handling for graceful termination
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create the CSV file
    FILE *csv_file = create_csv_file(commands, command_count, output_folder);
    if (csv_file == NULL) {
        close(fd);
        free(device);
        return 1;
    }

    // Main loop to send commands at the specified interval
    while (running) {
        process_commands(fd, commands, command_count, csv_file);

        // Sleep for the specified interval
        usleep(interval * 1000); // Convert milliseconds to microseconds
    }

    // Close the CSV file
    fclose(csv_file);

    // Close the serial port
    close(fd);

    // Free dynamically allocated memory
    free(device);
    if (file_mode) {
        for (int i = 0; i < command_count; i++) {
            free(commands[i]);
        }
        free(output_folder);
    }

    return 0;
}

// Function to configure the serial port
int configure_serial_port(int fd, int baud_rate) {
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    // Set Baud Rate
    cfsetospeed(&tty, baud_rate);
    cfsetispeed(&tty, baud_rate);

    // 8N1 Mode
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

// Function to send AT command
int send_at_command(int fd, const char *command) {
    // Create a buffer to hold the command with '\r' added
    char cmd_with_cr[256];
    snprintf(cmd_with_cr, sizeof(cmd_with_cr), "%s\r", command);

    // Send the command
    ssize_t n = write(fd, cmd_with_cr, strlen(cmd_with_cr));
    if (n < 0) {
        perror("write");
        return -1;
    }

    return 0;
}

// Function to flush the serial port
void flush_serial_port(int fd) {
    tcflush(fd, TCIOFLUSH);
}

// Function to read response
int read_response(int fd, char *response, size_t max_len) {
    size_t total_read = 0;
    int bytes_read;

    // Loop until we either read enough data or reach the end of the timeout
    while (total_read < max_len - 1) {
        bytes_read = read(fd, response + total_read, max_len - total_read - 1);

        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Resource temporarily unavailable, continue reading
                continue;
            }
            perror("read");
            return -1;
        } else if (bytes_read == 0) {
            // No more data available
            break;
        }

        total_read += bytes_read;

        // Check if the end of the response is reached
        if (strchr(response, '\n') != NULL) {
            break;
        }
    }

    response[total_read] = '\0'; // Null-terminate the string
    return total_read;
}

// Function to request modem property (send AT command and get the response)
int request_modem_property(int fd, const char *command, char *response, size_t max_len) {
    // Send the AT command
    if (send_at_command(fd, command) != 0) {
        return -1;
    }

    // Read the response
    if (read_response(fd, response, max_len) < 0) {
        return -1;
    }

    return 0;
}

// Function to process a list of commands
void process_commands(int fd, char *commands[], int count, FILE *csv_file) {
    char response[1024];
    char *responses[count];

    for (int i = 0; i < count; i++) {
        responses[i] = malloc(1024); // Allocate memory for each response
        if (responses[i] == NULL) {
            perror("Error allocating memory for response");
            return;
        }
    }

    // Send each command and store responses
    for (int i = 0; i < count; i++) {
        const char *at_command = commands[i];

        // Flush the serial port before sending a new command
        flush_serial_port(fd);

        // Send the AT command and get the response
        if (request_modem_property(fd, at_command, responses[i], sizeof(response)) != 0) {
            fprintf(stderr, "Error processing command '%s'\n", at_command);
            strcpy(responses[i], "ERROR"); // Indicate an error
        }
    }

    // Get the current timestamp
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[256];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    // Print and write each response
    printf("Timestamp: %s\n", timestamp);
    fprintf(csv_file, "\"%s\"", timestamp);

    for (int i = 0; i < count; i++) {
        printf("Command: %s\nResponse: %s\n\n", commands[i], responses[i]);
        fprintf(csv_file, ",\"%s\"", responses[i]);
        free(responses[i]); // Free the memory after use
    }

    // Write a newline to the CSV file to finish the row
    fprintf(csv_file, "\n");
}

// Function to read configuration from a file
int read_config_file(const char *filename, char **device, int *baud_rate, char *commands[], int max_count, int *interval, char **output_folder) {
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

    // Default output folder
    *output_folder = strdup(DEFAULT_OUTPUT_FOLDER);
    if (*output_folder == NULL) {
        perror("Error allocating memory for output folder");
        fclose(file);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        // Remove trailing newline characters
        line[strcspn(line, "\r\n")] = '\0';

        // Convert to lowercase for case-insensitive comparison
        char lower_line[256];
        strncpy(lower_line, line, sizeof(lower_line));
        lower_line[sizeof(lower_line) - 1] = '\0';
        to_lowercase(lower_line);

        if (strncmp(lower_line, "device:", 7) == 0) {
            free(*device);
            *device = strdup(line + 7); // Preserve the case of the device path
            if (*device == NULL) {
                perror("Error allocating memory for device");
                fclose(file);
                return -1;
            }
            trim_whitespace(*device);
            remove_surrounding_quotes(*device);
        } else if (strncmp(lower_line, "baud_rate:", 10) == 0) {
            *baud_rate = atoi(line + 10);
        } else if (strncmp(lower_line, "commands:", 9) == 0) {
            in_commands_block = 1; // Start reading commands block
            continue;
        } else if (strncmp(lower_line, "interval:", 9) == 0) {
            *interval = atoi(line + 9);
        } else if (strncmp(lower_line, "output_folder:", 14) == 0) {
            free(*output_folder);
            *output_folder = strdup(line + 14);
            if (*output_folder == NULL) {
                perror("Error allocating memory for output folder");
                fclose(file);
                return -1;
            }
            trim_whitespace(*output_folder);
            remove_surrounding_quotes(*output_folder);
        }

        if (in_commands_block) {
            if (line[0] == '}') {
                break; // End of commands block
            } else if (line[0] == '{') {
                continue; // Skip opening brace
            }

            // Concatenate lines in the commands block
            strcat(command_buffer, line);

            // Split commands separated by commas
            char *cmd = strtok(command_buffer, ",");
            while (cmd != NULL) {
                // Trim whitespace around commands
                trim_whitespace(cmd);

                // Remove surrounding quotes if present
                remove_surrounding_quotes(cmd);

                // Store the command
                commands[count] = strdup(cmd);
                if (commands[count] == NULL) {
                    perror("Error allocating memory for command");
                    fclose(file);
                    return -1;
                }
                count++;
                cmd = strtok(NULL, ",");

                if (count >= max_count) {
                    break; // Stop if we reach the maximum number of commands
                }
            }

            // Clear the command buffer for the next line
            memset(command_buffer, 0, sizeof(command_buffer));
        }
    }

    fclose(file);
    return count;
}

// Function to convert string to lowercase
void to_lowercase(char *str) {
    for (char *p = str; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
}

// Function to trim whitespace from the start and end of a string
void trim_whitespace(char *str) {
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0)  // All spaces?
        return;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    // Null terminate after the last non-space character
    *(end + 1) = '\0';
}

// Function to remove surrounding quotes from a string
void remove_surrounding_quotes(char *str) {
    size_t len = strlen(str);
    if (len > 1 && str[0] == '\"' && str[len - 1] == '\"') {
        // Shift the string to remove the quotes
        memmove(str, str + 1, len - 1);
        str[len - 2] = '\0';
    }
}

// Function to create a CSV file with the current timestamp
FILE *create_csv_file(char *commands[], int count, const char *output_folder) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // Create output folder if it doesn't exist
    struct stat st = {0};
    if (stat(output_folder, &st) == -1) {
        if (mkdir(output_folder, 0700) != 0) {
            perror("Error creating output folder");
            return NULL;
        }
    }

    // Format the filename based on the current date and time
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

    // Write the header row to the CSV file
    fprintf(file, "Timestamp");
    for (int i = 0; i < count; i++) {
        fprintf(file, ",\"%s\"", commands[i]);
    }
    fprintf(file, "\n");
    return file;
}

// Signal handler for graceful termination
void signal_handler(int signum) {
    running = 0;
}
