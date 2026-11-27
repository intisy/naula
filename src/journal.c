#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "layout_apply_internal.h"
#include "layout_internal.h"
#include "naula/json.h"
#include "naula/memory.h"

#define JOURNAL_FORMAT 1
#define JOURNAL_NAME_ATTEMPTS 100

static const char *const JOURNAL_FIX = "check that the root's .naula-journal folder is a real folder naula can write to";
static const char *const MALFORMED_FIX = "restore the journal naula wrote, or lay the root out again with naula sync";
static const char *const SEQUENCE_FIX = "move the file naula did not name out of .naula-journal, then run naula sync "
                                        "again";
static const char *const DUPLICATE_FIX = "keep the journal naula wrote and move the copy out of .naula-journal, then "
                                         "run naula sync again";
static const char *const EXHAUSTED_FIX = "move the finished journals out of .naula-journal, keeping any unfinished "
                                         "one, then run naula sync again";

static naula_status refuse_journal(naula_refusal *refusal, const char *path, const char *why) {
    naula_refuse(refusal, "journal-invalid", MALFORMED_FIX, "the journal %s %s", path, why);
    return NAULA_REFUSED;
}

static naula_status append_line(journal *journal, cJSON *line, naula_refusal *refusal) {
    char *text = line == NULL ? NULL : cJSON_PrintUnformatted(line);
    cJSON_Delete(line);
    if (text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t length = strlen(text);
    char *terminated = naula_memory_allocate(length + 2);
    if (terminated == NULL) {
        cJSON_free(text);
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(terminated, text, length);
    terminated[length] = '\n';
    terminated[length + 1] = '\0';
    cJSON_free(text);
    unsigned long error = 0;
    naula_status status = disk_append(journal->appender, terminated, length + 1, &error);
    naula_memory_release(terminated);
    return status == NAULA_REFUSED ? apply_refuse_disk(refusal, "journal-unwritable", JOURNAL_FIX, "cannot write",
                                                       journal->path, error)
                                   : status;
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

static cJSON *planned_line(size_t index, const naula_layout_step *step) {
    cJSON *line = cJSON_CreateObject();
    if (line == NULL) {
        return NULL;
    }
    int built = cJSON_AddNumberToObject(line, "step", (double)index) != NULL && add_text(line, "state", "planned") &&
                cJSON_AddNumberToObject(line, "kind", (double)step->kind) != NULL &&
                add_text(line, "path", step->path) && add_text(line, "target", step->target) &&
                add_text(line, "content", step->content) &&
                add_text(line, "code", step->code[0] == '\0' ? NULL : step->code);
    if (!built) {
        cJSON_Delete(line);
        return NULL;
    }
    return line;
}

static int is_digit(char character) {
    return character >= '0' && character <= '9';
}

/** `<UTC yyyymmddThhmmssZ>.jsonl`. */
static int is_stamp_suffix(const char *stamp) {
    if (strlen(stamp) != 16 + 6 || strcmp(stamp + 16, ".jsonl") != 0 || stamp[8] != 'T' || stamp[15] != 'Z') {
        return 0;
    }
    for (size_t index = 0; index < 15; index++) {
        if (index != 8 && !is_digit(stamp[index])) {
            return 0;
        }
    }
    return 1;
}

journal_name_kind journal_name_read(const char *name, unsigned long long *sequence) {
    size_t digits = 0;
    while (is_digit(name[digits])) {
        digits++;
    }
    if (digits == 0 || name[digits] != '-' || !is_stamp_suffix(name + digits + 1)) {
        return JOURNAL_NAME_OTHER;
    }
    if (digits < JOURNAL_SEQUENCE_WIDTH || digits > JOURNAL_SEQUENCE_DIGITS_LIMIT) {
        return JOURNAL_NAME_UNREADABLE;
    }
    unsigned long long value = 0;
    for (size_t index = 0; index < digits; index++) {
        value = value * 10u + (unsigned long long)(name[index] - '0');
    }
    *sequence = value;
    return JOURNAL_NAME_SEQUENCED;
}

void journal_listing_release(journal_listing *listing) {
    for (size_t index = 0; index < listing->count; index++) {
        naula_memory_release(listing->journals[index].name);
    }
    naula_memory_release(listing->journals);
    listing->journals = NULL;
    listing->count = 0;
}

static int by_sequence_descending(const void *left, const void *right) {
    unsigned long long left_sequence = ((const journal_name *)left)->sequence;
    unsigned long long right_sequence = ((const journal_name *)right)->sequence;
    return left_sequence < right_sequence ? 1 : (left_sequence > right_sequence ? -1 : 0);
}

/** Moves every journal's name out of `names` into `listing`, which has room for all of them. */
static naula_status take_journals(const char *folder, char **names, size_t count, journal_listing *listing,
                                  naula_refusal *refusal) {
    for (size_t index = 0; index < count; index++) {
        unsigned long long sequence = 0;
        journal_name_kind kind = journal_name_read(names[index], &sequence);
        if (kind == JOURNAL_NAME_UNREADABLE) {
            naula_refuse(refusal, "journal-sequence-invalid", SEQUENCE_FIX,
                         "%s in %s is named as a journal, with a sequence naula cannot read", names[index], folder);
            return NAULA_REFUSED;
        }
        if (kind == JOURNAL_NAME_SEQUENCED) {
            listing->journals[listing->count].name = names[index];
            listing->journals[listing->count].sequence = sequence;
            listing->count++;
            names[index] = NULL;
        }
    }
    return NAULA_OK;
}

/** Two journals of one sequence, as a copied journal leaves, in a listing already in sequence order. */
static naula_status refuse_shared_sequence(const char *folder, const journal_listing *listing,
                                           naula_refusal *refusal) {
    for (size_t index = 1; index < listing->count; index++) {
        if (listing->journals[index].sequence == listing->journals[index - 1].sequence) {
            naula_refuse(refusal, "journal-sequence-duplicate", DUPLICATE_FIX,
                         "the journals %s and %s in %s carry one sequence, so neither can be read as the newer",
                         listing->journals[index - 1].name, listing->journals[index].name, folder);
            return NAULA_REFUSED;
        }
    }
    return NAULA_OK;
}

naula_status journal_list(const char *folder, journal_listing *listing, naula_refusal *refusal) {
    listing->journals = NULL;
    listing->count = 0;
    char **names = NULL;
    size_t count = 0;
    unsigned long error = 0;
    naula_status status = disk_names(folder, &names, &count, &error);
    if (status == NAULA_REFUSED) {
        return apply_refuse_disk(refusal, "root-unreadable", JOURNAL_FIX, "cannot list", folder, error);
    }
    if (status == NAULA_OK && count > 0) {
        listing->journals = naula_memory_allocate(count * sizeof *listing->journals);
        status = listing->journals == NULL ? NAULA_OUT_OF_MEMORY : take_journals(folder, names, count, listing, refusal);
    }
    disk_names_release(names, count);
    if (status == NAULA_OK && listing->count > 1) {
        qsort(listing->journals, listing->count, sizeof *listing->journals, by_sequence_descending);
        status = refuse_shared_sequence(folder, listing, refusal);
    }
    if (status != NAULA_OK) {
        journal_listing_release(listing);
    }
    return status;
}

static naula_status largest_sequence(const char *folder, unsigned long long *largest, naula_refusal *refusal) {
    journal_listing listing;
    *largest = 0;
    naula_status status = journal_list(folder, &listing, refusal);
    if (status == NAULA_OK && listing.count > 0 && listing.journals[0].sequence >= JOURNAL_SEQUENCE_LARGEST) {
        naula_refuse(refusal, "journal-sequence-exhausted", EXHAUSTED_FIX,
                     "the journal %s in %s holds the largest sequence a journal's name carries, so no newer journal "
                     "can be named",
                     listing.journals[0].name, folder);
        status = NAULA_REFUSED;
    }
    if (status == NAULA_OK && listing.count > 0) {
        *largest = listing.journals[0].sequence;
    }
    journal_listing_release(&listing);
    return status;
}

/** Named by a sequence one past the largest in the folder; the time in the name is only information. */
static naula_status open_new(const char *folder, journal *journal, naula_refusal *refusal) {
    char stamp[17];
    disk_utc_stamp(stamp);
    unsigned long long largest = 0;
    naula_status listed = largest_sequence(folder, &largest, refusal);
    if (listed != NAULA_OK) {
        return listed;
    }
    for (int attempt = 1; attempt <= JOURNAL_NAME_ATTEMPTS && largest + (unsigned)attempt <= JOURNAL_SEQUENCE_LARGEST;
         attempt++) {
        char name[64];
        snprintf(name, sizeof name, "%0*llu-%s.jsonl", JOURNAL_SEQUENCE_WIDTH, largest + (unsigned long long)attempt,
                 stamp);
        char *path = layout_join(folder, name);
        if (path == NULL) {
            return NAULA_OUT_OF_MEMORY;
        }
        unsigned long error = 0;
        disk_kind kind = DISK_MISSING;
        naula_status status = disk_kind_of(path, &kind, &error);
        if (status == NAULA_OK && kind == DISK_MISSING) {
            status = disk_appender_open(path, 1, &journal->appender, &error);
            if (status == NAULA_OK) {
                journal->path = path;
                return NAULA_OK;
            }
        }
        if (status != NAULA_OK) {
            status = status == NAULA_REFUSED ? apply_refuse_disk(refusal, "journal-unwritable", JOURNAL_FIX,
                                                                 "cannot create", path, error)
                                             : status;
            naula_memory_release(path);
            return status;
        }
        naula_memory_release(path);
    }
    naula_refuse(refusal, "journal-unwritable", JOURNAL_FIX, "every journal name for %s in %s is taken", stamp, folder);
    return NAULA_REFUSED;
}

typedef struct opening {
    char *text;
    size_t length;
} opening;

/** Adds `line` and its newline to the opening text, taking ownership of `line`. */
static naula_status add_line(opening *built, cJSON *line) {
    char *text = line == NULL ? NULL : cJSON_PrintUnformatted(line);
    cJSON_Delete(line);
    if (text == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    size_t length = strlen(text);
    char *larger = naula_memory_reallocate(built->text, built->length + length + 2);
    if (larger == NULL) {
        cJSON_free(text);
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(larger + built->length, text, length);
    larger[built->length + length] = '\n';
    larger[built->length + length + 1] = '\0';
    built->text = larger;
    built->length += length + 1;
    cJSON_free(text);
    return NAULA_OK;
}

static cJSON *header_line(const char *root, const naula_layout_plan *plan) {
    cJSON *header = cJSON_CreateObject();
    int built = header != NULL && cJSON_AddNumberToObject(header, "journal", JOURNAL_FORMAT) != NULL &&
                add_text(header, "root", root) &&
                cJSON_AddNumberToObject(header, "steps", (double)plan->step_count) != NULL;
    if (!built) {
        cJSON_Delete(header);
        return NULL;
    }
    return header;
}

/**
 * The header and the whole plan go to the disk in one append, built before the file is made, so a journal holds
 * either its whole plan or nothing; one that holds nothing never ran.
 */
naula_status journal_start(const char *root, const naula_layout_plan *plan, journal *journal, naula_refusal *refusal) {
    journal->appender = NULL;
    journal->path = NULL;
    opening built = {NULL, 0};
    naula_status status = add_line(&built, header_line(root, plan));
    for (size_t index = 0; status == NAULA_OK && index < plan->step_count; index++) {
        status = add_line(&built, planned_line(index, &plan->steps[index]));
    }
    char *folder = status == NAULA_OK ? layout_join(root, APPLY_JOURNAL_FOLDER) : NULL;
    if (status == NAULA_OK && folder == NULL) {
        status = NAULA_OUT_OF_MEMORY;
    }
    if (status == NAULA_OK) {
        status = open_new(folder, journal, refusal);
    }
    naula_memory_release(folder);
    unsigned long error = 0;
    if (status == NAULA_OK) {
        status = disk_append(journal->appender, built.text, built.length, &error);
        if (status == NAULA_REFUSED) {
            status = apply_refuse_disk(refusal, "journal-unwritable", JOURNAL_FIX, "cannot write", journal->path,
                                       error);
        }
    }
    naula_memory_release(built.text);
    if (status != NAULA_OK) {
        journal_close(journal);
    }
    return status;
}

naula_status journal_mark(journal *journal, size_t step, const char *state, const char *code, const char *previous,
                          naula_refusal *refusal) {
    cJSON *line = cJSON_CreateObject();
    int built = line != NULL && cJSON_AddNumberToObject(line, "step", (double)step) != NULL &&
                add_text(line, "state", state) && add_text(line, "code", code) && add_text(line, "previous", previous);
    if (!built) {
        cJSON_Delete(line);
        return NAULA_OUT_OF_MEMORY;
    }
    return append_line(journal, line, refusal);
}

naula_status journal_finish(journal *journal, naula_refusal *refusal) {
    cJSON *line = cJSON_CreateObject();
    if (line == NULL || !add_text(line, "state", "finished")) {
        cJSON_Delete(line);
        return NAULA_OUT_OF_MEMORY;
    }
    return append_line(journal, line, refusal);
}

naula_status journal_forgotten(journal *journal, const char *path, const char *target, naula_refusal *refusal) {
    cJSON *line = cJSON_CreateObject();
    if (line == NULL || !add_text(line, "state", "forgotten") || !add_text(line, "path", path) ||
        !add_text(line, "target", target)) {
        cJSON_Delete(line);
        return NAULA_OUT_OF_MEMORY;
    }
    return append_line(journal, line, refusal);
}

naula_status journal_stop(journal *journal, naula_refusal *refusal) {
    cJSON *line = cJSON_CreateObject();
    if (line == NULL || !add_text(line, "state", "stopped")) {
        cJSON_Delete(line);
        return NAULA_OUT_OF_MEMORY;
    }
    return append_line(journal, line, refusal);
}

void journal_close(journal *journal) {
    disk_appender_close(journal->appender);
    naula_memory_release(journal->path);
    journal->appender = NULL;
    journal->path = NULL;
}

naula_status journal_reopen(const char *path, int ends_mid_line, journal *journal, naula_refusal *refusal) {
    journal->path = naula_memory_duplicate_text(path);
    if (journal->path == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    unsigned long error = 0;
    naula_status status = disk_appender_open(path, 0, &journal->appender, &error);
    if (status == NAULA_OK && ends_mid_line) {
        status = disk_append(journal->appender, "\n", 1, &error);
    }
    if (status != NAULA_OK) {
        status = status == NAULA_REFUSED ? apply_refuse_disk(refusal, "journal-unwritable", JOURNAL_FIX,
                                                             "cannot append to", path, error)
                                         : status;
        journal_close(journal);
    }
    return status;
}

static void drop_plan(journal_contents *contents) {
    for (size_t index = 0; contents->plan.steps != NULL && index < contents->plan.step_count; index++) {
        naula_memory_release(contents->plan.steps[index].path);
        naula_memory_release(contents->plan.steps[index].target);
        naula_memory_release(contents->plan.steps[index].content);
    }
    naula_memory_release(contents->plan.steps);
    contents->plan.steps = NULL;
    contents->plan.step_count = 0;
}

void journal_contents_release(journal_contents *contents) {
    naula_memory_release(contents->root);
    drop_plan(contents);
    naula_memory_release(contents->progress);
    memset(contents, 0, sizeof *contents);
}

static naula_status copy_member(const cJSON *line, const char *name, char **out) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(line, name);
    *out = NULL;
    if (!cJSON_IsString(member)) {
        return NAULA_OK;
    }
    *out = naula_memory_duplicate_text(member->valuestring);
    return *out == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
}

static int whole_number(const cJSON *member, double limit, size_t *value) {
    if (!cJSON_IsNumber(member) || member->valuedouble < 0 || member->valuedouble >= limit ||
        member->valuedouble != (double)(size_t)member->valuedouble) {
        return 0;
    }
    *value = (size_t)member->valuedouble;
    return 1;
}

static naula_status read_header(const cJSON *line, const char *path, journal_contents *contents,
                                naula_refusal *refusal) {
    size_t format = 0;
    size_t steps = 0;
    const cJSON *root = cJSON_GetObjectItemCaseSensitive(line, "root");
    if (!whole_number(cJSON_GetObjectItemCaseSensitive(line, "journal"), 1e9, &format) || format != JOURNAL_FORMAT ||
        !cJSON_IsString(root) || !whole_number(cJSON_GetObjectItemCaseSensitive(line, "steps"), 1e9, &steps)) {
        return refuse_journal(refusal, path, "does not start with a journal header naula writes");
    }
    contents->root = naula_memory_duplicate_text(root->valuestring);
    contents->plan.size = sizeof contents->plan;
    contents->plan.step_count = steps;
    contents->plan.steps = steps == 0 ? NULL : naula_memory_allocate(steps * sizeof *contents->plan.steps);
    contents->progress = naula_memory_allocate(steps + 1);
    if (contents->root == NULL || (steps > 0 && contents->plan.steps == NULL) || contents->progress == NULL) {
        return NAULA_OUT_OF_MEMORY;
    }
    if (steps > 0) {
        memset(contents->plan.steps, 0, steps * sizeof *contents->plan.steps);
    }
    memset(contents->progress, 0, steps + 1);
    return NAULA_OK;
}

static naula_status read_planned(const cJSON *line, size_t index, const char *path, journal_contents *contents,
                                 naula_refusal *refusal) {
    naula_layout_step *step = &contents->plan.steps[index];
    size_t kind = 0;
    if (step->size != 0 || !whole_number(cJSON_GetObjectItemCaseSensitive(line, "kind"), NAULA_REPORT + 1.0, &kind) ||
        kind == 0) {
        return refuse_journal(refusal, path, "plans a step twice or plans a step of no kind naula knows");
    }
    step->size = sizeof *step;
    step->kind = (uint32_t)kind;
    naula_status status = copy_member(line, "path", &step->path);
    if (status == NAULA_OK) {
        status = copy_member(line, "target", &step->target);
    }
    if (status == NAULA_OK) {
        status = copy_member(line, "content", &step->content);
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(line, "code");
    if (status == NAULA_OK && cJSON_IsString(code)) {
        snprintf(step->code, sizeof step->code, "%s", code->valuestring);
    }
    return status;
}

static unsigned char progress_of(const char *state) {
    if (strcmp(state, "intended") == 0) {
        return JOURNAL_INTENDED;
    }
    if (strcmp(state, "done") == 0) {
        return JOURNAL_DONE;
    }
    if (strcmp(state, "recorded") == 0) {
        return JOURNAL_RECORDED;
    }
    return JOURNAL_NONE;
}

static naula_status read_line(const cJSON *line, const char *path, journal_contents *contents,
                              naula_refusal *refusal) {
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(line, "state");
    if (!cJSON_IsObject(line) || !cJSON_IsString(state)) {
        return refuse_journal(refusal, path, "holds a line that is not a step's state");
    }
    if (strcmp(state->valuestring, "planned") != 0) {
        contents->progressed = 1;
    }
    if (strcmp(state->valuestring, "finished") == 0) {
        contents->finished = 1;
        return NAULA_OK;
    }
    if (strcmp(state->valuestring, "stopped") == 0) {
        contents->stopped = 1;
        return NAULA_OK;
    }
    if (strcmp(state->valuestring, "forgotten") == 0) {
        return NAULA_OK;
    }
    contents->stopped = 0;
    size_t index = 0;
    if (!whole_number(cJSON_GetObjectItemCaseSensitive(line, "step"), (double)contents->plan.step_count, &index)) {
        return refuse_journal(refusal, path, "names a step the plan does not hold");
    }
    if (strcmp(state->valuestring, "planned") == 0) {
        return read_planned(line, index, path, contents, refusal);
    }
    unsigned char progress = progress_of(state->valuestring);
    if (contents->progress[index] < progress) {
        contents->progress[index] = progress;
    }
    return NAULA_OK;
}

static naula_status parse_line(const uint8_t *start, size_t length, cJSON **line, naula_refusal *refusal,
                               const char *path) {
    const char *reason = NULL;
    naula_status status = naula_json_parse(start, length, line, &reason);
    return status == NAULA_MALFORMED ? refuse_journal(refusal, path, "holds a line that is not JSON") : status;
}

static naula_status read_lines(const uint8_t *bytes, size_t length, const char *path, journal_contents *contents,
                               naula_refusal *refusal) {
    size_t start = 0;
    int seen_header = 0;
    naula_status status = NAULA_OK;
    while (status == NAULA_OK && start < length) {
        const uint8_t *newline = memchr(bytes + start, '\n', length - start);
        size_t end = newline == NULL ? length : (size_t)(newline - bytes);
        if (newline == NULL) {
            contents->ends_mid_line = 1;
            break;
        }
        cJSON *line = NULL;
        status = parse_line(bytes + start, end - start, &line, refusal, path);
        if (status == NAULA_OK) {
            status = seen_header ? read_line(line, path, contents, refusal) : read_header(line, path, contents, refusal);
            seen_header = 1;
        }
        cJSON_Delete(line);
        start = end + 1;
    }
    if (status == NAULA_OK && !seen_header) {
        contents->stopped = 1;
        return NAULA_OK;
    }
    for (size_t index = 0; status == NAULA_OK && index < contents->plan.step_count; index++) {
        if (contents->plan.steps[index].size == 0) {
            if (contents->progressed) {
                return refuse_journal(refusal, path, "does not plan every step it counts");
            }
            drop_plan(contents);
            contents->stopped = 1;
            return NAULA_OK;
        }
    }
    return status;
}

naula_status journal_load(const char *path, journal_contents *contents, naula_refusal *refusal) {
    memset(contents, 0, sizeof *contents);
    uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned long error = 0;
    naula_status status = disk_file_read(path, &bytes, &length, &error);
    if (status == NAULA_REFUSED || status == NAULA_NOT_FOUND) {
        status = apply_refuse_disk(refusal, "journal-unreadable", MALFORMED_FIX, "cannot read", path, error);
    }
    if (status == NAULA_OK) {
        status = read_lines(bytes, length, path, contents, refusal);
    }
    naula_memory_release(bytes);
    if (status != NAULA_OK) {
        journal_contents_release(contents);
    }
    return status;
}
