#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#define BUFFER_SIZE 4048 // buffer size for storing file info

typedef struct {//structure to hold info about each file
    char *src_path;   //source file path
    char *dest_path;  //destination file path
    int src_fd;       //source file descriptor
    int dest_fd;      //destination file descriptor
} file_info_t;

typedef struct {// circular buffer structure for holding files to copied
    file_info_t buffer[BUFFER_SIZE]; // array to store file info
    int front, rear;  // indices for front and rear of the buffer
    int count;        // count of items in the buffer
    pthread_mutex_t mutex; // mutex for synchronizing access to the buffer
    pthread_cond_t cond_full;  // buffer is full
    pthread_cond_t cond_empty; // buffer is empty
} buffer_t;

buffer_t buffer; // buffer instance
int buffer_size, num_workers; // buffer size and number of worker threads
char *src_dir, *dest_dir; // source and destination directory paths
int done_flag = 0; // flag to indicate directory processing is done
pthread_barrier_t barrier; // barrier for synchronizing worker threads

void error_handler(const char *msg) {//func for error handling
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

void init_buffer() {//func for initialize buffer
    buffer.front = buffer.rear = 0;
    buffer.count = 0;
    pthread_mutex_init(&buffer.mutex, NULL);
    pthread_cond_init(&buffer.cond_full, NULL);
    pthread_cond_init(&buffer.cond_empty, NULL);
}

void enqueue(file_info_t file_info) {//function for add file info to buffer
    pthread_mutex_lock(&buffer.mutex);

    while (buffer.count == buffer_size) // wait if buffer is full
        pthread_cond_wait(&buffer.cond_full, &buffer.mutex);

    // add file information to the buffer
    buffer.buffer[buffer.rear] = file_info;
    buffer.rear = (buffer.rear + 1) % BUFFER_SIZE;
    buffer.count++;

    // signal that the buffer is not empty
    pthread_cond_signal(&buffer.cond_empty);
    pthread_mutex_unlock(&buffer.mutex);
}

file_info_t dequeue() {//func for remove and return file info from buffer
    file_info_t file_info;
    pthread_mutex_lock(&buffer.mutex);

    //wait if buffer is empty
    while (buffer.count == 0)
        pthread_cond_wait(&buffer.cond_empty, &buffer.mutex);

    //remove file information from the buffer
    file_info = buffer.buffer[buffer.front];
    buffer.front = (buffer.front + 1) % BUFFER_SIZE;
    buffer.count--;

    //signal that the buffer is not full
    pthread_cond_signal(&buffer.cond_full);
    pthread_mutex_unlock(&buffer.mutex);

    return file_info;
}

void free_file_info(file_info_t *file_info) {//func for free memory allocated for file info
    free(file_info->src_path);
    free(file_info->dest_path);
}

void *manager(void *arg) {//manager thread function to read directory contents and enqueue diles
    DIR *src_dir_stream;
    struct dirent *src_dir_entry;
    struct stat src_stat;
    file_info_t file_info = {0};
    char *src_path, *dest_path;
    size_t src_path_len, dest_path_len;

    src_dir_stream = opendir(src_dir); //open the source directory
    if (src_dir_stream == NULL) {
        perror("Failed to open source directory");
        error_handler("Failed to open source directory");
    }

    while ((src_dir_entry = readdir(src_dir_stream)) != NULL) { //read entries in the source directory
        if (strcmp(src_dir_entry->d_name, ".") == 0 || strcmp(src_dir_entry->d_name, "..") == 0)
            continue;

        //calculate lengths for source and destination paths
        src_path_len = strlen(src_dir) + strlen(src_dir_entry->d_name) + 2;
        dest_path_len = strlen(dest_dir) + strlen(src_dir_entry->d_name) + 2;

        //allocate memory for source and destination paths
        src_path = malloc(src_path_len);
        dest_path = malloc(dest_path_len);

        if (src_path == NULL || dest_path == NULL) {
            error_handler("Memory allocation failed");
        }

        //create full paths for source and destination
        snprintf(src_path, src_path_len, "%s/%s", src_dir, src_dir_entry->d_name);
        snprintf(dest_path, dest_path_len, "%s/%s", dest_dir, src_dir_entry->d_name);

        if (stat(src_path, &src_stat) == -1) { //get status of the source file
            perror("Failed to get source file status");
            free(src_path);
            free(dest_path);
            continue;
        }

        if (S_ISDIR(src_stat.st_mode)) { //check if the entry is a directory
            if (mkdir(dest_path, src_stat.st_mode) == -1 && errno != EEXIST) { //handle directories recursively
                perror("Failed to create destination directory");
                free(src_path);
                free(dest_path);
                continue;
            }

            //temporarily change directories and recurse
            char *prev_src_dir = src_dir;
            char *prev_dest_dir = dest_dir;
            src_dir = src_path;
            dest_dir = dest_path;
            manager(NULL); //recurse for the subdirectory
            src_dir = prev_src_dir;
            dest_dir = prev_dest_dir;

            free(src_path);
            free(dest_path);
        } else {
            file_info.src_fd = open(src_path, O_RDONLY); //open source and destination files
            if (file_info.src_fd == -1) {
                perror("Failed to open source file");
                free(src_path);
                free(dest_path);
                continue;
            }

            file_info.dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode);
            if (file_info.dest_fd == -1) {
                perror("Failed to open destination file");
                close(file_info.src_fd);
                free(src_path);
                free(dest_path);
                continue;
            }

            file_info.src_path = src_path;
            file_info.dest_path = dest_path;

            enqueue(file_info); //enqueue the file information
        }
    }

    closedir(src_dir_stream); //close the source directory

    done_flag = 1; //set flag for processing is complete
    pthread_cond_broadcast(&buffer.cond_empty); //wake up all worker threads

    return NULL;
}

void *worker(void *arg) {//worker thread function for copy files from source to destination
    file_info_t file_info;
    char buffer_temp[BUFSIZ];
    ssize_t bytes_read, bytes_written;

    pthread_barrier_wait(&barrier); //wait at the barrier

    while (!done_flag || buffer.count > 0) { //process files until flag is set and buffer is empty
        file_info = dequeue();
        while ((bytes_read = read(file_info.src_fd, buffer_temp, BUFSIZ)) > 0) { //copy data from source file to destination file
            bytes_written = write(file_info.dest_fd, buffer_temp, bytes_read);
            if (bytes_written != bytes_read)
                error_handler("Failed to write to destination file");
        }

        if (bytes_read == -1)
            error_handler("Failed to read from source file");

        //close file descriptors
        close(file_info.src_fd);
        close(file_info.dest_fd);

        printf("Copied %s to %s\n", file_info.src_path, file_info.dest_path);
        free_file_info(&file_info); //free allocated memory for file paths
    }

    return NULL;
}

int main(int argc, char *argv[]) {//main func for set up and start manager and worker threads
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <buffer_size> <num_workers> <source_dir> <dest_dir>\n", argv[0]);
        return 1;
    }

    buffer_size = atoi(argv[1]); //get buffer size
    num_workers = atoi(argv[2]); //get number of worker threads
    src_dir = argv[3]; //get source directory path
    dest_dir = argv[4]; //get destination directory path

    init_buffer(); // init the buffer

    pthread_barrier_init(&barrier, NULL, num_workers + 1); //initialize the barrier

    pthread_t manager_thread, worker_threads[num_workers]; //create manager and worker threads
    pthread_create(&manager_thread, NULL, manager, NULL);
    for (int i = 0; i < num_workers; i++)
        pthread_create(&worker_threads[i], NULL, worker, NULL);

    pthread_barrier_wait(&barrier); //wait at the barrier in the main thread

    pthread_join(manager_thread, NULL); //wait for manager and worker threads to complete
    for (int i = 0; i < num_workers; i++)
        pthread_join(worker_threads[i], NULL);

    pthread_barrier_destroy(&barrier); //destroy the barrier

    return 0;
}