#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdio.h>
#include <stddef.h>

#define MAX_BOOKS      100
#define MAX_MEMBERS    100
#define MAX_LIBRARIANS 20

#define ID_SIZE      20
#define TITLE_SIZE   80
#define NAME_SIZE    50
#define COURSE_SIZE  10
#define BOOK_ID_PREFIX      "BK"
#define MEMBER_ID_PREFIX    "ALU"
#define LIBRARIAN_ID_PREFIX "LIB"

typedef struct {
    char book_id[ID_SIZE];
    char title[TITLE_SIZE];
    char author[NAME_SIZE];
} Book;


typedef struct {
    char member_id[ID_SIZE];
    char full_name[NAME_SIZE];
    char course_code[COURSE_SIZE];

    /* Not in members.txt: kept with the saved chain state. */
    long fines_owed;
    long fines_paid;
} Member;


typedef struct {
    char librarian_id[ID_SIZE];
    char full_name[NAME_SIZE];
    char role[12];
    char pin_hash[65];
} Librarian;


typedef struct {
    Book books[MAX_BOOKS];
    int book_count;
    Member members[MAX_MEMBERS];
    int member_count;
    Librarian librarians[MAX_LIBRARIANS];
    int librarian_count;
} Registry;


int load_books(const char *filename, Registry *registry);
int load_members(const char *filename, Registry *registry);
int load_librarians(const char *filename, Registry *registry);

/* Each returns the index, or -1 if the ID is not registered. */
int find_book(const Registry *registry, const char *book_id);
int find_member(const Registry *registry, const char *member_id);
int find_librarian(const Registry *registry, const char *librarian_id);

/* Add to the registry and append to the file; return 0 on failure. */
int register_book(const char *filename, Registry *registry, const Book *book);
int register_member(const char *filename, Registry *registry, const Member *member);

/* Input validation. */
int id_has_format(const char *id, const char *prefix);
/* Non-empty, shorter than size, and free of the ',' and '|' separators. */
int text_field_ok(const char *text, size_t size);

/* Shared reader for the comma-separated registry files. */
#define CSV_LINE_SIZE 512
int csv_next_record(FILE *file, const char *filename, int *line_no,
                    char line[CSV_LINE_SIZE], char *fields[], int field_count);
int csv_append_line(const char *filename, const char *line);

#endif
