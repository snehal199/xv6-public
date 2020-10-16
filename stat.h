#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

struct stat {
  short type;  // Type of file
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short nlink; // Number of links to file
  uint size;   // Size of file in bytes
};

struct uproc {
  uint sz;                     // Size of process memory (bytes)
  int pid;                     // Process ID
  int killed;                  // If non-zero, have been killed
  char name[16];               // Process name (debugging)
  int ppid;                    // Parent process ID
};
