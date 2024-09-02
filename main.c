#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <ctype.h>

// Default values
#define DEFAULT_DEVICE "/dev/ttyUSB3"
#define DEFAULT_BAUD_RATE 115200

// Function prototypes
int configure_serial_port(int fd, int baud_rate);
int send_at_command(int fd, const char *command);
void flush_serial_port(int fd);
int read_response(int fd, char *response, size_t max_len);
int request_modem_property(int fd, const char *command, char *response, size_t max_len);
void process_commands(int fd, char *commands[], int count);
int read_config_file(const char *filename, char **device, int *baud_rate, char *commands[], int max_count);
void to_lowercase(char *str);

int main(int argc, char *argv[]) {
    char *device = NULL;
    int baud_rate = DEFAULT_BAUD_RATE; // Default baud rate
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
        int count = read_config_file(filename, &device, &baud_rate, commands, sizeof(commands) / sizeof(commands[0]));
        if (count < 0) {
            fprintf(stderr, "Error reading configuration from file '%s'\n", filename);
            free(device);
            return 1;
        }
        command_count = count;
    }

    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    // int fd = open("/dev/ttyUSB3", O_RDWR | O_NOCTTY | O_NDELAY);
    printf(">>>>>>%s\n", device);
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

    // Process commands
    if (command_count > 0) {
        process_commands(fd, commands, command_count);
    } else {
        fprintf(stderr, "No AT commands provided.\n");
    }

    // Close the serial port
    close(fd);

    // Free dynamically allocated memory
    free(device);
    for (int i = 0; i < command_count; i++) {
        free(commands[i]);
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
void process_commands(int fd, char *commands[], int count) {
    char response[1024];

    for (int i = 0; i < count; i++) {
        const char *at_command = commands[i];

        // Flush the serial port before sending a new command
        flush_serial_port(fd);

        // Send the AT command and get the response
        if (request_modem_property(fd, at_command, response, sizeof(response)) != 0) {
            fprintf(stderr, "Error processing command '%s'\n", at_command);
            continue;
        }

        // Print the modem response
        printf("Response to '%s':\n%s\n", at_command, response);
    }
}

// Function to read configuration from a file
int read_config_file(const char *filename, char **device, int *baud_rate, char *commands[], int max_count) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error opening configuration file");
        return -1;
    }

    char line[256];
    int count = 0;
    int in_commands_block = 0;
    char command_buffer[1024] = {0}; // Buffer to accumulate commands across lines

    while (fgets(line, sizeof(line), file) != NULL) {
        // Remove trailing newline characters
        line[strcspn(line, "\r\n")] = '\0';

        // Convert to lowercase for case-insensitive comparison
        to_lowercase(line);

        if (strncmp(line, "device:", 7) == 0) {
            free(*device);
            *device = strdup(line + 7);
            if (*device == NULL) {
                perror("Error allocating memory for device");
                fclose(file);
                return -1;
            }
        } else if (strncmp(line, "baud_rate:", 10) == 0) {
            *baud_rate = atoi(line + 10);
        } else if (strncmp(line, "commands:", 9) == 0) {
            in_commands_block = 1; // Start reading commands block
            continue;
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
                while (isspace((unsigned char)*cmd)) cmd++;
                char *end = cmd + strlen(cmd) - 1;
                while (end > cmd && isspace((unsigned char)*end)) end--;
                end[1] = '\0';

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
