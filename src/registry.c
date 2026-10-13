#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "registry.h"

/* Removes spaces and '\r' at both ends. */
static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    return text;
}

int id_has_format(const char *id, const char *prefix)
{
    size_t prefix_len = strlen(prefix);

    if (strncmp(id, prefix, prefix_len) != 0) {
        return 0;
    }

    size_t digits = strlen(id + prefix_len);
    if (digits < 3 || digits > 6) {
        return 0;
    }
    for (const char *p = id + prefix_len; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

int text_field_ok(const char *text, size_t size)
{
    return text[0] != '\0' && strlen(text) < size &&
           strchr(text, ',') == NULL && strchr(text, '|') == NULL;
}

/* Splits the next good line into fields; returns 0 at end of file. */
int csv_next_record(FILE *file, const char *filename, int *line_no,
                    char line[CSV_LINE_SIZE], char *fields[], int field_count)
{
    while (fgets(line, CSV_LINE_SIZE, file) != NULL) {
        (*line_no)++;

        if (strchr(line, '\n') == NULL && !feof(file)) {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF);
            printf("WARNING: %s line %d is too long - skipped.\n", filename, *line_no);
            continue;
        }

        char *cursor = trim(line);
        if (*cursor == '\0') {
            continue;
        }

        int found = 0;
        while (found < field_count) {
            fields[found++] = cursor;

            char *comma = (found < field_count) ? strchr(cursor, ',') : NULL;
            if (comma == NULL) {
                break;
            }
            *comma = '\0';
            cursor = comma + 1;
        }

        int ok = (found == field_count);
        for (int i = 0; ok && i < field_count; i++) {
            fields[i] = trim(fields[i]);
            /* '|' separates fields in the saved chain file. */
            if (fields[i][0] == '\0' || strchr(fields[i], '|') != NULL) {
                ok = 0;
            }
        }

        if (!ok) {
            printf("WARNING: %s line %d should have %d non-empty fields without '|' - skipped.\n",
                   filename, *line_no, field_count);
            continue;
        }

        return 1;
    }

    return 0;
}

/* Appends one line, first ending the last line if it has no newline. */
int csv_append_line(const char *filename, const char *line)
{
    FILE *file = fopen(filename, "a+");
    if (file == NULL) {
        return 0;
    }

    int needs_newline = 0;
    if (fseek(file, -1, SEEK_END) == 0) {
        needs_newline = fgetc(file) != '\n';
    }
    fseek(file, 0, SEEK_END);

    int ok = fprintf(file, "%s%s\n", needs_newline ? "\n" : "", line) > 0;
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static int field_fits(const char *filename, int line_no, const char *name,
                      const char *value, size_t size)
{
    if (strlen(value) < size) {
        return 1;
    }
    printf("WARNING: %s line %d: %s is longer than %zu characters - skipped.\n",
           filename, line_no, name, size - 1);
    return 0;
}

static int id_fits(const char *filename, int line_no, const char *id, const char *prefix)
{
    if (id_has_format(id, prefix)) {
        return 1;
    }
    printf("WARNING: %s line %d: ID '%s' must be %s followed by 3-6 digits - skipped.\n",
           filename, line_no, id, prefix);
    return 0;
}

static FILE *open_registry(const char *filename, const char *what)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        printf("ERROR: Could not open '%s'. %s registry is required to start the system.\n",
               filename, what);
    }
    return file;
}

static int finish_registry(FILE *file, const char *filename, int count, const char *what)
{
    fclose(file);

    if (count == 0) {
        printf("ERROR: '%s' is empty or badly formatted. No %s loaded.\n", filename, what);
    }
    return count;
}

/* books.txt: book_id,title,author */
int load_books(const char *filename, Registry *registry)
{
    FILE *file = open_registry(filename, "Book");
    if (file == NULL) {
        return 0;
    }

    Book *books = registry->books;
    char line[CSV_LINE_SIZE];
    char *fields[3];
    int line_no = 0;
    int count = 0;

    registry->book_count = 0;
    while (csv_next_record(file, filename, &line_no, line, fields, 3)) {
        if (!id_fits(filename, line_no, fields[0], BOOK_ID_PREFIX) ||
            !field_fits(filename, line_no, "title",   fields[1], sizeof(books[0].title)) ||
            !field_fits(filename, line_no, "author",  fields[2], sizeof(books[0].author))) {
            continue;
        }
        if (find_book(registry, fields[0]) != -1) {
            printf("WARNING: %s line %d: duplicate book ID %s - skipped.\n", filename, line_no, fields[0]);
            continue;
        }
        if (count == MAX_BOOKS) {
            printf("WARNING: %s has more than %d books - the rest were ignored.\n", filename, MAX_BOOKS);
            break;
        }

        strcpy(books[count].book_id, fields[0]);
        strcpy(books[count].title, fields[1]);
        strcpy(books[count].author, fields[2]);
        registry->book_count = ++count;
    }

    return finish_registry(file, filename, count, "books");
}

/* members.txt: member_id,full_name,course_code */
int load_members(const char *filename, Registry *registry)
{
    FILE *file = open_registry(filename, "Member");
    if (file == NULL) {
        return 0;
    }

    Member *members = registry->members;
    char line[CSV_LINE_SIZE];
    char *fields[3];
    int line_no = 0;
    int count = 0;

    registry->member_count = 0;
    while (csv_next_record(file, filename, &line_no, line, fields, 3)) {
        if (!id_fits(filename, line_no, fields[0], MEMBER_ID_PREFIX) ||
            !field_fits(filename, line_no, "full_name",   fields[1], sizeof(members[0].full_name)) ||
            !field_fits(filename, line_no, "course_code", fields[2], sizeof(members[0].course_code))) {
            continue;
        }
        if (find_member(registry, fields[0]) != -1) {
            printf("WARNING: %s line %d: duplicate member ID %s - skipped.\n", filename, line_no, fields[0]);
            continue;
        }
        if (count == MAX_MEMBERS) {
            printf("WARNING: %s has more than %d members - the rest were ignored.\n", filename, MAX_MEMBERS);
            break;
        }

        memset(&members[count], 0, sizeof(Member));
        strcpy(members[count].member_id, fields[0]);
        strcpy(members[count].full_name, fields[1]);
        strcpy(members[count].course_code, fields[2]);
        registry->member_count = ++count;
    }

    return finish_registry(file, filename, count, "members");
}

/* librarians.txt: librarian_id,full_name,role,pin_hash */
int load_librarians(const char *filename, Registry *registry)
{
    FILE *file = open_registry(filename, "Librarian");
    if (file == NULL) {
        return 0;
    }

    Librarian *librarians = registry->librarians;
    char line[CSV_LINE_SIZE];
    char *fields[4];
    int line_no = 0;
    int count = 0;

    registry->librarian_count = 0;
    while (csv_next_record(file, filename, &line_no, line, fields, 4)) {
        if (!id_fits(filename, line_no, fields[0], LIBRARIAN_ID_PREFIX) ||
            !field_fits(filename, line_no, "full_name", fields[1], sizeof(librarians[0].full_name))) {
            continue;
        }
        if (strcmp(fields[2], "ADMIN") != 0 && strcmp(fields[2], "LIBRARIAN") != 0) {
            printf("WARNING: %s line %d: role must be ADMIN or LIBRARIAN - skipped.\n", filename, line_no);
            continue;
        }
        if (strlen(fields[3]) != 64) {
            printf("WARNING: %s line %d: pin_hash must be 64 hex characters - skipped.\n", filename, line_no);
            continue;
        }
        if (find_librarian(registry, fields[0]) != -1) {
            printf("WARNING: %s line %d: duplicate librarian ID %s - skipped.\n", filename, line_no, fields[0]);
            continue;
        }
        if (count == MAX_LIBRARIANS) {
            printf("WARNING: %s has more than %d librarians - the rest were ignored.\n", filename, MAX_LIBRARIANS);
            break;
        }

        strcpy(librarians[count].librarian_id, fields[0]);
        strcpy(librarians[count].full_name, fields[1]);
        strcpy(librarians[count].role, fields[2]);
        strcpy(librarians[count].pin_hash, fields[3]);
        registry->librarian_count = ++count;
    }

    return finish_registry(file, filename, count, "librarians");
}

int find_book(const Registry *registry, const char *book_id)
{
    for (int i = 0; i < registry->book_count; i++) {
        /* Exact match only. */
        if (strcmp(registry->books[i].book_id, book_id) == 0) {
            return i;
        }
    }

    return -1;
}

int find_member(const Registry *registry, const char *member_id)
{
    for (int i = 0; i < registry->member_count; i++) {
        if (strcmp(registry->members[i].member_id, member_id) == 0) {
            return i;
        }
    }

    return -1;
}

int find_librarian(const Registry *registry, const char *librarian_id)
{
    for (int i = 0; i < registry->librarian_count; i++) {
        if (strcmp(registry->librarians[i].librarian_id, librarian_id) == 0) {
            return i;
        }
    }

    return -1;
}

int register_book(const char *filename, Registry *registry, const Book *book)
{
    char line[CSV_LINE_SIZE];

    if (registry->book_count == MAX_BOOKS || find_book(registry, book->book_id) != -1) {
        return 0;
    }

    snprintf(line, sizeof(line), "%s,%s,%s", book->book_id, book->title, book->author);
    if (!csv_append_line(filename, line)) {
        return 0;
    }
    registry->books[registry->book_count++] = *book;
    return 1;
}

int register_member(const char *filename, Registry *registry, const Member *member)
{
    char line[CSV_LINE_SIZE];

    if (registry->member_count == MAX_MEMBERS || find_member(registry, member->member_id) != -1) {
        return 0;
    }

    snprintf(line, sizeof(line), "%s,%s,%s", member->member_id, member->full_name,
             member->course_code);
    if (!csv_append_line(filename, line)) {
        return 0;
    }
    registry->members[registry->member_count++] = *member;
    return 1;
}
