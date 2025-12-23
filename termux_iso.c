#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <fnmatch.h>

#define SECTOR_SIZE 2048
#define SYS_AREA_SECTORS 16

typedef struct Node Node;

typedef struct {
    Node **items;
    size_t count;
    size_t capacity;
} NodeList;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

struct Node {
    char *name;
    char *source_path;
    char *iso_name;
    bool is_dir;
    uint32_t extent;
    uint32_t size;
    uint32_t dir_size;
    uint32_t dir_number;
    NodeList children;
    Node *parent;
    bool synthetic;
    time_t mtime;
};

typedef struct {
    const char *system_id;
    const char *volume_id;
    const char *volume_set_id;
    const char *publisher_id;
    const char *preparer_id;
    const char *application_id;
} VolumeInfo;

static const char *g_root_path = NULL;
static size_t g_root_path_len = 0;
static StringList g_excludes;
static bool g_use_fixed_time = false;
static time_t g_fixed_time = 0;

static char *xstrdup(const char *str);

static void list_init(NodeList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void string_list_init(StringList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void list_push(NodeList *list, Node *node) {
    if (list->count == list->capacity) {
        size_t next = list->capacity == 0 ? 8 : list->capacity * 2;
        Node **items = realloc(list->items, next * sizeof(Node *));
        if (!items) {
            perror("realloc");
            exit(1);
        }
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = node;
}

static void string_list_push(StringList *list, const char *value) {
    if (list->count == list->capacity) {
        size_t next = list->capacity == 0 ? 8 : list->capacity * 2;
        char **items = realloc(list->items, next * sizeof(char *));
        if (!items) {
            perror("realloc");
            exit(1);
        }
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = xstrdup(value);
}

static void *xcalloc(size_t count, size_t size) {
    void *ptr = calloc(count, size);
    if (!ptr) {
        perror("calloc");
        exit(1);
    }
    return ptr;
}

static char *xstrdup(const char *str) {
    char *dup = strdup(str);
    if (!dup) {
        perror("strdup");
        exit(1);
    }
    return dup;
}

static char iso_sanitize_char(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return c;
    }
    return '_';
}

static unsigned digits_count(unsigned value) {
    unsigned count = 1;
    while (value >= 10) {
        value /= 10;
        count++;
    }
    return count;
}

static void split_name(const char *name, char *base, size_t base_len, char *ext, size_t ext_len) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        dot = NULL;
    }
    size_t i = 0;
    const char *src = name;
    while (*src && (dot ? src < dot : true) && i + 1 < base_len) {
        base[i++] = iso_sanitize_char(*src++);
    }
    base[i] = '\0';
    if (dot) {
        dot++;
        size_t j = 0;
        while (*dot && j + 1 < ext_len) {
            ext[j++] = iso_sanitize_char(*dot++);
        }
        ext[j] = '\0';
    } else {
        ext[0] = '\0';
    }
}

static char *build_iso_name(const char *name, bool is_dir, unsigned suffix) {
    char base[9] = {0};
    char ext[4] = {0};
    if (is_dir) {
        size_t i = 0;
        for (const char *src = name; *src && i + 1 < sizeof(base); ++src) {
            base[i++] = iso_sanitize_char(*src);
        }
        base[i] = '\0';
        ext[0] = '\0';
    } else {
        split_name(name, base, sizeof(base), ext, sizeof(ext));
    }
    if (base[0] == '\0') {
        strcpy(base, "_");
    }

    unsigned suffix_len = suffix ? (1 + digits_count(suffix)) : 0;
    unsigned base_limit = is_dir ? 8 : 8;
    if (suffix_len >= base_limit) {
        suffix_len = base_limit - 1;
    }
    unsigned allowed = base_limit - suffix_len;
    if (strlen(base) > allowed) {
        base[allowed] = '\0';
    }
    if (suffix) {
        char suffix_buf[8];
        snprintf(suffix_buf, sizeof(suffix_buf), "~%u", suffix);
        strncat(base, suffix_buf, sizeof(base) - strlen(base) - 1);
    }

    if (is_dir || ext[0] == '\0') {
        return xstrdup(base);
    }
    char composed[13];
    snprintf(composed, sizeof(composed), "%s.%s", base, ext);
    return xstrdup(composed);
}

static Node *node_create(const char *name, const char *path, bool is_dir) {
    Node *node = xcalloc(1, sizeof(Node));
    node->name = xstrdup(name);
    node->source_path = path ? xstrdup(path) : NULL;
    node->iso_name = NULL;
    node->is_dir = is_dir;
    list_init(&node->children);
    return node;
}

static bool iso_name_exists(Node *parent, const char *name) {
    for (size_t i = 0; i < parent->children.count; ++i) {
        if (strcmp(parent->children.items[i]->iso_name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void ensure_unique_iso_name(Node *parent, Node *child) {
    if (!parent) {
        if (!child->iso_name) {
            child->iso_name = build_iso_name(child->name, child->is_dir, 0);
        }
        return;
    }
    unsigned suffix = 0;
    char *candidate = NULL;
    do {
        free(candidate);
        candidate = build_iso_name(child->name, child->is_dir, suffix);
        suffix++;
    } while (iso_name_exists(parent, candidate));
    free(child->iso_name);
    child->iso_name = candidate;
}

static void add_child(Node *parent, Node *child) {
    child->parent = parent;
    ensure_unique_iso_name(parent, child);
    list_push(&parent->children, child);
}

static void free_node(Node *node) {
    for (size_t i = 0; i < node->children.count; ++i) {
        free_node(node->children.items[i]);
    }
    free(node->children.items);
    free(node->name);
    free(node->source_path);
    free(node->iso_name);
    free(node);
}

static void free_string_list(StringList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
}

static time_t choose_time(time_t mtime) {
    return g_use_fixed_time ? g_fixed_time : mtime;
}

static const char *relative_path(const char *path) {
    if (g_root_path && strncmp(path, g_root_path, g_root_path_len) == 0) {
        const char *rel = path + g_root_path_len;
        if (*rel == '/') {
            rel++;
        }
        return rel;
    }
    return path;
}

static bool is_excluded(const char *path, const char *name) {
    if (g_excludes.count == 0) {
        return false;
    }
    const char *rel = relative_path(path);
    for (size_t i = 0; i < g_excludes.count; ++i) {
        const char *pattern = g_excludes.items[i];
        if (fnmatch(pattern, rel, FNM_PATHNAME) == 0 || fnmatch(pattern, name, 0) == 0) {
            return true;
        }
    }
    return false;
}

static void collect_directory(Node *node) {
    DIR *dir = opendir(node->source_path);
    if (!dir) {
        fprintf(stderr, "Failed to open directory %s: %s\n", node->source_path, strerror(errno));
        exit(1);
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", node->source_path, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            fprintf(stderr, "Failed to stat %s: %s\n", path, strerror(errno));
            exit(1);
        }
        if (is_excluded(path, entry->d_name)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            Node *child = node_create(entry->d_name, path, true);
            child->mtime = choose_time(st.st_mtime);
            add_child(node, child);
            collect_directory(child);
        } else if (S_ISREG(st.st_mode)) {
            Node *child = node_create(entry->d_name, path, false);
            child->size = (uint32_t)st.st_size;
            child->mtime = choose_time(st.st_mtime);
            add_child(node, child);
        }
    }
    closedir(dir);
}

static int compare_nodes(const void *a, const void *b) {
    const Node *left = *(const Node **)a;
    const Node *right = *(const Node **)b;
    if (left->is_dir != right->is_dir) {
        return left->is_dir ? -1 : 1;
    }
    return strcmp(left->iso_name, right->iso_name);
}

static void sort_children(Node *node) {
    if (!node->is_dir) {
        return;
    }
    if (node->children.count > 1) {
        qsort(node->children.items, node->children.count, sizeof(Node *), compare_nodes);
    }
    for (size_t i = 0; i < node->children.count; ++i) {
        sort_children(node->children.items[i]);
    }
}

static uint32_t record_length_for_name(size_t name_len) {
    uint32_t length = 33 + (uint32_t)name_len;
    if ((name_len & 1) == 0) {
        return length;
    }
    return length + 1;
}

static void compute_directory_sizes(Node *node) {
    if (!node->is_dir) {
        return;
    }
    uint32_t size = 0;
    size += record_length_for_name(1);
    size += record_length_for_name(1);
    for (size_t i = 0; i < node->children.count; ++i) {
        Node *child = node->children.items[i];
        const char *name = child->iso_name;
        char versioned[64];
        if (!child->is_dir) {
            snprintf(versioned, sizeof(versioned), "%s;1", name);
            size += record_length_for_name(strlen(versioned));
        } else {
            size += record_length_for_name(strlen(name));
        }
        if (child->is_dir) {
            compute_directory_sizes(child);
        }
    }
    node->dir_size = size;
}

static void assign_directory_numbers(Node *node, uint32_t *next) {
    if (!node->is_dir) {
        return;
    }
    node->dir_number = (*next)++;
    for (size_t i = 0; i < node->children.count; ++i) {
        assign_directory_numbers(node->children.items[i], next);
    }
}

static uint32_t count_path_table_size(Node *node) {
    if (!node->is_dir) {
        return 0;
    }
    uint32_t size = 0;
    size_t name_len = node->parent == NULL ? 1 : strlen(node->iso_name);
    uint32_t record_len = 8 + (uint32_t)name_len;
    if (name_len & 1) {
        record_len++;
    }
    size += record_len;
    for (size_t i = 0; i < node->children.count; ++i) {
        size += count_path_table_size(node->children.items[i]);
    }
    return size;
}

static void assign_extents(Node *node, uint32_t *next_extent) {
    if (node->is_dir) {
        uint32_t sectors = (node->dir_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
        node->extent = *next_extent;
        *next_extent += sectors;
        for (size_t i = 0; i < node->children.count; ++i) {
            assign_extents(node->children.items[i], next_extent);
        }
    } else {
        uint32_t sectors = (node->size + SECTOR_SIZE - 1) / SECTOR_SIZE;
        node->extent = *next_extent;
        *next_extent += sectors;
    }
}

static void write_733(uint8_t *buf, uint32_t value) {
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);
    buf[3] = (uint8_t)((value >> 24) & 0xFF);
    buf[4] = buf[3];
    buf[5] = buf[2];
    buf[6] = buf[1];
    buf[7] = buf[0];
}

static void write_723(uint8_t *buf, uint16_t value) {
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = buf[1];
    buf[3] = buf[0];
}

static void write_timestamp(uint8_t *buf, time_t timestamp) {
    struct tm *tm_info = gmtime(&timestamp);
    if (!tm_info) {
        memset(buf, 0, 7);
        return;
    }
    buf[0] = (uint8_t)(tm_info->tm_year);
    buf[1] = (uint8_t)(tm_info->tm_mon + 1);
    buf[2] = (uint8_t)(tm_info->tm_mday);
    buf[3] = (uint8_t)(tm_info->tm_hour);
    buf[4] = (uint8_t)(tm_info->tm_min);
    buf[5] = (uint8_t)(tm_info->tm_sec);
    buf[6] = 0;
}

static uint32_t write_directory_record(uint8_t *buf, Node *node, const char *name, uint8_t flags) {
    size_t name_len = strlen(name);
    uint32_t length = record_length_for_name(name_len);
    buf[0] = (uint8_t)length;
    buf[1] = 0;
    write_733(buf + 2, node->extent);
    write_733(buf + 10, node->is_dir ? node->dir_size : node->size);
    write_timestamp(buf + 18, node->mtime);
    buf[25] = flags;
    buf[26] = 0;
    buf[27] = 0;
    write_723(buf + 28, 1);
    buf[32] = (uint8_t)name_len;
    memcpy(buf + 33, name, name_len);
    if (name_len & 1) {
        buf[33 + name_len] = 0;
    }
    return length;
}

static void write_directory(FILE *iso, Node *node) {
    if (!node->is_dir) {
        return;
    }
    uint32_t size = node->dir_size;
    uint32_t sectors = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t bytes = sectors * SECTOR_SIZE;
    uint8_t *buffer = xcalloc(bytes, 1);
    uint32_t offset = 0;

    offset += write_directory_record(buffer + offset, node, "\0", 0x02);
    Node *parent = node->parent ? node->parent : node;
    offset += write_directory_record(buffer + offset, parent, "\1", 0x02);

    for (size_t i = 0; i < node->children.count; ++i) {
        Node *child = node->children.items[i];
        if (child->is_dir) {
            offset += write_directory_record(buffer + offset, child, child->iso_name, 0x02);
        } else {
            char name[64];
            snprintf(name, sizeof(name), "%s;1", child->iso_name);
            offset += write_directory_record(buffer + offset, child, name, 0x00);
        }
    }

    fseek(iso, (long)node->extent * SECTOR_SIZE, SEEK_SET);
    fwrite(buffer, 1, bytes, iso);
    free(buffer);

    for (size_t i = 0; i < node->children.count; ++i) {
        write_directory(iso, node->children.items[i]);
    }
}

static void write_path_table_entry(uint8_t *buf, size_t *offset, Node *node, bool big_endian) {
    uint8_t name_len = node->parent == NULL ? 1 : (uint8_t)strlen(node->iso_name);
    buf[(*offset)++] = name_len;
    buf[(*offset)++] = 0;
    uint32_t extent = node->extent;
    if (big_endian) {
        buf[(*offset)++] = (uint8_t)((extent >> 24) & 0xFF);
        buf[(*offset)++] = (uint8_t)((extent >> 16) & 0xFF);
        buf[(*offset)++] = (uint8_t)((extent >> 8) & 0xFF);
        buf[(*offset)++] = (uint8_t)(extent & 0xFF);
    } else {
        buf[(*offset)++] = (uint8_t)(extent & 0xFF);
        buf[(*offset)++] = (uint8_t)((extent >> 8) & 0xFF);
        buf[(*offset)++] = (uint8_t)((extent >> 16) & 0xFF);
        buf[(*offset)++] = (uint8_t)((extent >> 24) & 0xFF);
    }
    uint16_t parent = node->parent ? (uint16_t)node->parent->dir_number : 1;
    if (big_endian) {
        buf[(*offset)++] = (uint8_t)((parent >> 8) & 0xFF);
        buf[(*offset)++] = (uint8_t)(parent & 0xFF);
    } else {
        buf[(*offset)++] = (uint8_t)(parent & 0xFF);
        buf[(*offset)++] = (uint8_t)((parent >> 8) & 0xFF);
    }

    if (node->parent == NULL) {
        buf[(*offset)++] = 0;
    } else {
        memcpy(buf + *offset, node->iso_name, name_len);
        *offset += name_len;
    }
    if (name_len & 1) {
        buf[(*offset)++] = 0;
    }
}

static void write_path_table_recursive(uint8_t *buf, size_t *offset, Node *node, bool big_endian) {
    if (!node->is_dir) {
        return;
    }
    write_path_table_entry(buf, offset, node, big_endian);
    for (size_t i = 0; i < node->children.count; ++i) {
        write_path_table_recursive(buf, offset, node->children.items[i], big_endian);
    }
}

static void write_files(FILE *iso, Node *node) {
    if (node->is_dir) {
        for (size_t i = 0; i < node->children.count; ++i) {
            write_files(iso, node->children.items[i]);
        }
        return;
    }
    if (!node->source_path) {
        return;
    }
    FILE *src = fopen(node->source_path, "rb");
    if (!src) {
        fprintf(stderr, "Failed to open %s: %s\n", node->source_path, strerror(errno));
        exit(1);
    }
    fseek(iso, (long)node->extent * SECTOR_SIZE, SEEK_SET);
    uint8_t buffer[8192];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, read_bytes, iso);
    }
    fclose(src);

    uint32_t remainder = node->size % SECTOR_SIZE;
    if (remainder) {
        uint32_t padding = SECTOR_SIZE - remainder;
        uint8_t zeros[SECTOR_SIZE] = {0};
        fwrite(zeros, 1, padding, iso);
    }
}

static void write_padded_field(uint8_t *dest, size_t len, const char *value) {
    memset(dest, ' ', len);
    if (!value) {
        return;
    }
    size_t copy_len = strlen(value);
    if (copy_len > len) {
        copy_len = len;
    }
    memcpy(dest, value, copy_len);
}

static void write_pvd(FILE *iso, Node *root, uint32_t volume_sectors, uint32_t l_path_lba, uint32_t m_path_lba, uint32_t path_table_size, const VolumeInfo *info) {
    uint8_t sector[SECTOR_SIZE];
    memset(sector, 0, sizeof(sector));
    sector[0] = 1;
    memcpy(sector + 1, "CD001", 5);
    sector[6] = 1;
    write_padded_field(sector + 8, 32, info->system_id);
    write_padded_field(sector + 40, 32, info->volume_id);
    write_733(sector + 80, volume_sectors);
    write_723(sector + 120, 1);
    write_723(sector + 124, 1);
    write_723(sector + 128, SECTOR_SIZE);
    write_733(sector + 132, path_table_size);
    sector[140] = (uint8_t)(l_path_lba & 0xFF);
    sector[141] = (uint8_t)((l_path_lba >> 8) & 0xFF);
    sector[142] = (uint8_t)((l_path_lba >> 16) & 0xFF);
    sector[143] = (uint8_t)((l_path_lba >> 24) & 0xFF);
    sector[144] = 0;
    sector[145] = 0;
    sector[146] = 0;
    sector[147] = 0;
    sector[148] = (uint8_t)((m_path_lba >> 24) & 0xFF);
    sector[149] = (uint8_t)((m_path_lba >> 16) & 0xFF);
    sector[150] = (uint8_t)((m_path_lba >> 8) & 0xFF);
    sector[151] = (uint8_t)(m_path_lba & 0xFF);
    sector[152] = 0;
    sector[153] = 0;
    sector[154] = 0;
    sector[155] = 0;

    write_padded_field(sector + 190, 128, info->volume_set_id);
    write_padded_field(sector + 318, 128, info->publisher_id);
    write_padded_field(sector + 446, 128, info->preparer_id);
    write_padded_field(sector + 574, 128, info->application_id);

    uint8_t *root_record = sector + 156;
    write_directory_record(root_record, root, "\0", 0x02);

    fseek(iso, SYS_AREA_SECTORS * SECTOR_SIZE, SEEK_SET);
    fwrite(sector, 1, sizeof(sector), iso);
}

static void write_terminator(FILE *iso, uint32_t sector) {
    uint8_t buffer[SECTOR_SIZE];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 255;
    memcpy(buffer + 1, "CD001", 5);
    buffer[6] = 1;
    fseek(iso, (long)sector * SECTOR_SIZE, SEEK_SET);
    fwrite(buffer, 1, sizeof(buffer), iso);
}

static void write_boot_record(FILE *iso, uint32_t sector, uint32_t catalog_lba) {
    uint8_t buffer[SECTOR_SIZE];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0;
    memcpy(buffer + 1, "CD001", 5);
    buffer[6] = 1;
    memcpy(buffer + 7, "EL TORITO SPECIFICATION", 23);
    buffer[39] = 0;
    buffer[40] = (uint8_t)(catalog_lba & 0xFF);
    buffer[41] = (uint8_t)((catalog_lba >> 8) & 0xFF);
    buffer[42] = (uint8_t)((catalog_lba >> 16) & 0xFF);
    buffer[43] = (uint8_t)((catalog_lba >> 24) & 0xFF);
    fseek(iso, (long)sector * SECTOR_SIZE, SEEK_SET);
    fwrite(buffer, 1, sizeof(buffer), iso);
}

static void write_boot_catalog(FILE *iso, uint32_t lba, uint32_t boot_image_lba, uint16_t sectors) {
    uint8_t buffer[SECTOR_SIZE];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 1;
    buffer[1] = 0;
    buffer[2] = 0;
    buffer[3] = 0;
    memcpy(buffer + 4, "TERMUX-ISO", 10);
    buffer[0x1E] = 0x55;
    buffer[0x1F] = 0xAA;

    uint32_t sum = 0;
    for (size_t i = 0; i < 0x20; i += 2) {
        uint16_t word = (uint16_t)(buffer[i] | (buffer[i + 1] << 8));
        sum += word;
    }
    uint16_t checksum = (uint16_t)(0x10000 - (sum & 0xFFFF));
    buffer[0x1C] = (uint8_t)(checksum & 0xFF);
    buffer[0x1D] = (uint8_t)((checksum >> 8) & 0xFF);

    buffer[0x20] = 0x88;
    buffer[0x21] = 0;
    buffer[0x22] = 0;
    buffer[0x23] = 0;
    buffer[0x24] = 0;
    buffer[0x25] = 0;
    buffer[0x26] = (uint8_t)(sectors & 0xFF);
    buffer[0x27] = (uint8_t)((sectors >> 8) & 0xFF);
    buffer[0x28] = (uint8_t)(boot_image_lba & 0xFF);
    buffer[0x29] = (uint8_t)((boot_image_lba >> 8) & 0xFF);
    buffer[0x2A] = (uint8_t)((boot_image_lba >> 16) & 0xFF);
    buffer[0x2B] = (uint8_t)((boot_image_lba >> 24) & 0xFF);
    buffer[0x2C] = 0;
    buffer[0x2D] = 0;
    buffer[0x2E] = 0;
    buffer[0x2F] = 0;

    fseek(iso, (long)lba * SECTOR_SIZE, SEEK_SET);
    fwrite(buffer, 1, sizeof(buffer), iso);
}

static Node *add_synthetic_file(Node *root, const char *name, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "Failed to stat %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Boot image must be a regular file: %s\n", path);
        exit(1);
    }
    Node *node = node_create(name, path, false);
    node->size = (uint32_t)st.st_size;
    node->synthetic = true;
    node->mtime = choose_time(st.st_mtime);
    add_child(root, node);
    return node;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s -o output.iso [options] <input_dir>\n"
            "Options:\n"
            "  -b <boot.img>    Add El Torito boot image\n"
            "  -V <volume_id>   Set volume identifier\n"
            "  -S <system_id>   Set system identifier\n"
            "  -A <app_id>      Set application identifier\n"
            "  -P <publisher>   Set publisher identifier\n"
            "  -p <preparer>    Set data preparer identifier\n"
            "  -R <volset_id>   Set volume set identifier\n"
            "  -x <pattern>     Exclude path (fnmatch, repeatable)\n"
            "  -t <epoch>       Use fixed UNIX timestamp for all entries\n",
            prog);
}

int main(int argc, char **argv) {
    const char *output = NULL;
    const char *boot_image = NULL;
    const char *input_dir = NULL;
    VolumeInfo volume = {
        .system_id = "TERMUX-ISO",
        .volume_id = "TERMUX_ISO",
        .volume_set_id = "",
        .publisher_id = "",
        .preparer_id = "",
        .application_id = "TERMUX-ISO",
    };

    string_list_init(&g_excludes);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            boot_image = argv[++i];
        } else if (strcmp(argv[i], "-V") == 0 && i + 1 < argc) {
            volume.volume_id = argv[++i];
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            volume.system_id = argv[++i];
        } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            volume.application_id = argv[++i];
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            volume.publisher_id = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            volume.preparer_id = argv[++i];
        } else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            volume.volume_set_id = argv[++i];
        } else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            string_list_push(&g_excludes, argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            g_use_fixed_time = true;
            g_fixed_time = (time_t)strtoll(argv[++i], NULL, 10);
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 1;
        } else {
            input_dir = argv[i];
        }
    }

    if (!output || !input_dir) {
        usage(argv[0]);
        return 1;
    }

    struct stat st;
    if (stat(input_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Input directory not found: %s\n", input_dir);
        free_string_list(&g_excludes);
        return 1;
    }
    g_root_path = input_dir;
    g_root_path_len = strlen(input_dir);

    Node *root = node_create("ROOT", input_dir, true);
    root->mtime = choose_time(st.st_mtime);
    collect_directory(root);

    Node *catalog_node = NULL;
    Node *boot_node = NULL;
    if (boot_image) {
        catalog_node = node_create("BOOT.CAT", NULL, false);
        catalog_node->size = SECTOR_SIZE;
        catalog_node->synthetic = true;
        catalog_node->mtime = choose_time(time(NULL));
        add_child(root, catalog_node);
        boot_node = add_synthetic_file(root, "BOOTIMG.BIN", boot_image);
    }

    sort_children(root);
    compute_directory_sizes(root);

    uint32_t next_dir_number = 1;
    assign_directory_numbers(root, &next_dir_number);

    uint32_t path_table_size = count_path_table_size(root);
    uint32_t path_table_sectors = (path_table_size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    uint32_t descriptor_sectors = boot_image ? 3 : 2;
    uint32_t l_path_lba = SYS_AREA_SECTORS + descriptor_sectors;
    uint32_t m_path_lba = l_path_lba + path_table_sectors;
    uint32_t data_start = m_path_lba + path_table_sectors;

    uint32_t next_extent = data_start;
    assign_extents(root, &next_extent);

    uint32_t volume_sectors = next_extent;

    FILE *iso = fopen(output, "wb");
    if (!iso) {
        fprintf(stderr, "Failed to create %s: %s\n", output, strerror(errno));
        free_node(root);
        free_string_list(&g_excludes);
        return 1;
    }

    uint8_t zeros[SECTOR_SIZE] = {0};
    for (int i = 0; i < SYS_AREA_SECTORS; ++i) {
        fwrite(zeros, 1, SECTOR_SIZE, iso);
    }

    write_pvd(iso, root, volume_sectors, l_path_lba, m_path_lba, path_table_size, &volume);

    if (boot_image && catalog_node) {
        write_boot_record(iso, SYS_AREA_SECTORS + 1, catalog_node->extent);
        write_terminator(iso, SYS_AREA_SECTORS + 2);
    } else {
        write_terminator(iso, SYS_AREA_SECTORS + 1);
    }

    uint32_t path_table_bytes = path_table_sectors * SECTOR_SIZE;
    uint8_t *path_table = xcalloc(path_table_bytes, 1);
    size_t offset = 0;
    write_path_table_recursive(path_table, &offset, root, false);
    fseek(iso, (long)l_path_lba * SECTOR_SIZE, SEEK_SET);
    fwrite(path_table, 1, path_table_bytes, iso);
    memset(path_table, 0, path_table_bytes);
    offset = 0;
    write_path_table_recursive(path_table, &offset, root, true);
    fseek(iso, (long)m_path_lba * SECTOR_SIZE, SEEK_SET);
    fwrite(path_table, 1, path_table_bytes, iso);
    free(path_table);

    write_directory(iso, root);

    if (boot_image && catalog_node) {
        write_boot_catalog(iso, catalog_node->extent, boot_node->extent, (uint16_t)((boot_node->size + 511) / 512));
    }

    write_files(iso, root);

    fclose(iso);
    free_node(root);
    free_string_list(&g_excludes);
    return 0;
}
