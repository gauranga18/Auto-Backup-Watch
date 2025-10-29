/*
 * AutoBackupWatch - Directory File Versioning Tool
 * 
 * Watches a directory and creates versioned backups when files change.
 * Uses SHA-256 hashing to detect actual content changes (not just timestamp).
 * 
 * Compile: gcc autobackup.c -o autobackup -lssl -lcrypto
 * Usage: ./autobackup <directory_to_watch> [poll_interval_seconds]
 * Example: ./autobackup ./my_project 5
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <openssl/sha.h>

#define MAX_PATH 1024
#define MAX_FILES 1000
#define HASH_SIZE 65  // SHA-256 hex string + null terminator
#define BACKUP_DIR ".autobackup"

// Structure to track file state
typedef struct {
    char filename[MAX_PATH];
    char hash[HASH_SIZE];
    time_t last_modified;
    int version;
} FileState;

// Global file tracking
FileState tracked_files[MAX_FILES];
int file_count = 0;
char watch_directory[MAX_PATH];
char backup_directory[MAX_PATH];

// Function prototypes
void calculate_sha256(const char *filepath, char *output_hash);
void scan_directory();
void check_for_changes();
void create_backup(const char *filepath, int version);
int get_file_version(const char *filename);
void load_state();
void save_state();
int is_backup_file(const char *filename);
void create_backup_dir();
char* get_timestamp();
void print_status();

// Calculate SHA-256 hash of a file
void calculate_sha256(const char *filepath, char *output_hash) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        output_hash[0] = '\0';
        return;
    }

    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    unsigned char buffer[8192];
    size_t bytes;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        SHA256_Update(&sha256, buffer, bytes);
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);
    fclose(file);

    // Convert to hex string
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(output_hash + (i * 2), "%02x", hash[i]);
    }
    output_hash[64] = '\0';
}

// Check if filename is a backup file (contains _v and timestamp)
int is_backup_file(const char *filename) {
    return strstr(filename, "_v") != NULL && strstr(filename, "_backup_") != NULL;
}

// Get current version number for a file
int get_file_version(const char *filename) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(tracked_files[i].filename, filename) == 0) {
            return tracked_files[i].version;
        }
    }
    return 0;
}

// Get formatted timestamp
char* get_timestamp() {
    static char timestamp[32];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);
    return timestamp;
}

// Create backup directory if it doesn't exist
void create_backup_dir() {
    struct stat st = {0};
    if (stat(backup_directory, &st) == -1) {
        mkdir(backup_directory, 0755);
        printf("[AutoBackup] Created backup directory: %s\n", backup_directory);
    }
}

// Create a versioned backup of a file
void create_backup(const char *filepath, int version) {
    char backup_path[MAX_PATH];
    char *filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : (char*)filepath;
    
    // Extract name and extension
    char name[MAX_PATH], ext[MAX_PATH];
    char *dot = strrchr(filename, '.');
    
    if (dot) {
        strncpy(name, filename, dot - filename);
        name[dot - filename] = '\0';
        strcpy(ext, dot);
    } else {
        strcpy(name, filename);
        ext[0] = '\0';
    }
    
    // Create backup filename: name_v1_backup_20240101_120000.ext
    snprintf(backup_path, sizeof(backup_path), "%s/%s_v%d_backup_%s%s",
             backup_directory, name, version, get_timestamp(), ext);
    
    // Copy file
    FILE *src = fopen(filepath, "rb");
    FILE *dst = fopen(backup_path, "wb");
    
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        fprintf(stderr, "[ERROR] Failed to create backup: %s\n", backup_path);
        return;
    }
    
    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }
    
    fclose(src);
    fclose(dst);
    
    printf("✓ Backed up: %s → v%d (hash changed)\n", filename, version);
}

// Scan directory and update file tracking
void scan_directory() {
    DIR *dir = opendir(watch_directory);
    if (!dir) {
        fprintf(stderr, "[ERROR] Cannot open directory: %s\n", watch_directory);
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip directories, hidden files, and backup files
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.' || 
            is_backup_file(entry->d_name)) {
            continue;
        }
        
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", watch_directory, entry->d_name);
        
        struct stat file_stat;
        if (stat(filepath, &file_stat) != 0) {
            continue;
        }
        
        // Check if file is already tracked
        int found = 0;
        for (int i = 0; i < file_count; i++) {
            if (strcmp(tracked_files[i].filename, entry->d_name) == 0) {
                found = 1;
                break;
            }
        }
        
        // Add new file to tracking
        if (!found && file_count < MAX_FILES) {
            strcpy(tracked_files[file_count].filename, entry->d_name);
            calculate_sha256(filepath, tracked_files[file_count].hash);
            tracked_files[file_count].last_modified = file_stat.st_mtime;
            tracked_files[file_count].version = 1;
            printf("[AutoBackup] Now tracking: %s\n", entry->d_name);
            file_count++;
        }
    }
    
    closedir(dir);
}

// Check for file changes and create backups
void check_for_changes() {
    for (int i = 0; i < file_count; i++) {
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", 
                 watch_directory, tracked_files[i].filename);
        
        struct stat file_stat;
        if (stat(filepath, &file_stat) != 0) {
            // File deleted or inaccessible
            continue;
        }
        
        // Only check if modified time changed (optimization)
        if (file_stat.st_mtime <= tracked_files[i].last_modified) {
            continue;
        }
        
        // Calculate new hash
        char new_hash[HASH_SIZE];
        calculate_sha256(filepath, new_hash);
        
        // Compare hashes (detects actual content changes)
        if (strcmp(new_hash, tracked_files[i].hash) != 0) {
            // Content changed - create backup
            tracked_files[i].version++;
            create_backup(filepath, tracked_files[i].version);
            
            // Update tracking info
            strcpy(tracked_files[i].hash, new_hash);
            tracked_files[i].last_modified = file_stat.st_mtime;
            
            save_state();
        }
    }
}

// Save tracking state to file
void save_state() {
    char state_file[MAX_PATH];
    snprintf(state_file, sizeof(state_file), "%s/.autobackup_state", watch_directory);
    
    FILE *f = fopen(state_file, "w");
    if (!f) return;
    
    fprintf(f, "%d\n", file_count);
    for (int i = 0; i < file_count; i++) {
        fprintf(f, "%s|%s|%ld|%d\n",
                tracked_files[i].filename,
                tracked_files[i].hash,
                (long)tracked_files[i].last_modified,
                tracked_files[i].version);
    }
    
    fclose(f);
}

// Load tracking state from file
void load_state() {
    char state_file[MAX_PATH];
    snprintf(state_file, sizeof(state_file), "%s/.autobackup_state", watch_directory);
    
    FILE *f = fopen(state_file, "r");
    if (!f) return;
    
    fscanf(f, "%d\n", &file_count);
    for (int i = 0; i < file_count; i++) {
        long mtime;
        fscanf(f, "%[^|]|%[^|]|%ld|%d\n",
               tracked_files[i].filename,
               tracked_files[i].hash,
               &mtime,
               &tracked_files[i].version);
        tracked_files[i].last_modified = (time_t)mtime;
    }
    
    fclose(f);
    printf("[AutoBackup] Loaded state: tracking %d files\n", file_count);
}

// Print current status
void print_status() {
    printf("\n=== AutoBackupWatch Status ===\n");
    printf("Watching: %s\n", watch_directory);
    printf("Tracking %d file(s):\n", file_count);
    
    for (int i = 0; i < file_count; i++) {
        printf("  • %s (v%d)\n", 
               tracked_files[i].filename, 
               tracked_files[i].version);
    }
    printf("=============================\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <directory_to_watch> [poll_interval_seconds]\n", argv[0]);
        printf("Example: %s ./my_project 5\n", argv[0]);
        return 1;
    }
    
    // Get watch directory (remove trailing slash)
    strncpy(watch_directory, argv[1], MAX_PATH - 1);
    size_t len = strlen(watch_directory);
    if (len > 0 && watch_directory[len - 1] == '/') {
        watch_directory[len - 1] = '\0';
    }
    
    // Set poll interval (default 5 seconds)
    int poll_interval = (argc >= 3) ? atoi(argv[2]) : 5;
    if (poll_interval < 1) poll_interval = 5;
    
    // Validate directory
    struct stat st;
    if (stat(watch_directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[ERROR] Invalid directory: %s\n", watch_directory);
        return 1;
    }
    
    // Setup backup directory
    snprintf(backup_directory, sizeof(backup_directory), "%s/%s", 
             watch_directory, BACKUP_DIR);
    create_backup_dir();
    
    // Load previous state
    load_state();
    
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║        AutoBackupWatch - File Versioning       ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    printf("Watching directory: %s\n", watch_directory);
    printf("Backup location: %s\n", backup_directory);
    printf("Poll interval: %d seconds\n", poll_interval);
    printf("Press Ctrl+C to stop\n\n");
    
    // Initial scan
    printf("[AutoBackup] Scanning directory...\n");
    scan_directory();
    print_status();
    
    // Main monitoring loop
    printf("[AutoBackup] Monitoring for changes...\n\n");
    
    while (1) {
        sleep(poll_interval);
        
        scan_directory();      // Check for new files
        check_for_changes();   // Check for modifications
    }
    
    return 0;
}