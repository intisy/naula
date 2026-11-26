#include "schema_validate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schema_json.h"
#include "schema_pattern.h"

typedef struct validation {
    const cJSON *root;
    int unsupported;
    char *problem;
    size_t capacity;
} validation;

static const char *const IGNORED_KEYWORDS[] = {"$schema", "$id",  "title", "description",
                                               "$comment", "definitions", "then", "else"};

static int validate(validation *state, const cJSON *schema, const cJSON *instance);

static void unsupported(validation *state, const char *what, const char *detail) {
    if (!state->unsupported) {
        snprintf(state->problem, state->capacity, "%s %s", what, detail);
    }
    state->unsupported = 1;
}

static int is_ignored(const char *keyword) {
    for (size_t index = 0; index < sizeof IGNORED_KEYWORDS / sizeof IGNORED_KEYWORDS[0]; index++) {
        if (strcmp(keyword, IGNORED_KEYWORDS[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

int schema_json_equal(const cJSON *left, const cJSON *right) {
    if ((left->type & 0xFF) != (right->type & 0xFF)) {
        return 0;
    }
    if (cJSON_IsNumber(left)) {
        return left->valuedouble == right->valuedouble;
    }
    if (cJSON_IsString(left)) {
        size_t length = schema_json_string_length(left);
        return length == schema_json_string_length(right) && memcmp(left->valuestring, right->valuestring, length) == 0;
    }
    if (cJSON_IsArray(left)) {
        const cJSON *other = right->child;
        for (const cJSON *item = left->child; item != NULL; item = item->next, other = other->next) {
            if (other == NULL || !schema_json_equal(item, other)) {
                return 0;
            }
        }
        return other == NULL;
    }
    if (cJSON_IsObject(left)) {
        if (cJSON_GetArraySize(left) != cJSON_GetArraySize(right)) {
            return 0;
        }
        for (const cJSON *item = left->child; item != NULL; item = item->next) {
            const cJSON *other = cJSON_GetObjectItemCaseSensitive(right, item->string);
            if (other == NULL || !schema_json_equal(item, other)) {
                return 0;
            }
        }
    }
    return 1;
}

static int is_type(const cJSON *instance, const char *name) {
    if (strcmp(name, "object") == 0) {
        return cJSON_IsObject(instance);
    }
    if (strcmp(name, "array") == 0) {
        return cJSON_IsArray(instance);
    }
    if (strcmp(name, "string") == 0) {
        return cJSON_IsString(instance);
    }
    if (strcmp(name, "boolean") == 0) {
        return cJSON_IsBool(instance);
    }
    if (strcmp(name, "number") == 0) {
        return cJSON_IsNumber(instance);
    }
    if (strcmp(name, "integer") == 0) {
        return cJSON_IsNumber(instance) && floor(instance->valuedouble) == instance->valuedouble;
    }
    return strcmp(name, "null") == 0 && cJSON_IsNull(instance);
}

static size_t code_points(const cJSON *string) {
    size_t count = 0;
    size_t length = schema_json_string_length(string);
    const unsigned char *bytes = (const unsigned char *)string->valuestring;
    for (size_t index = 0; index < length; index++) {
        count += (bytes[index] & 0xC0u) != 0x80u;
    }
    return count;
}

static int check_pattern(validation *state, const cJSON *value, const cJSON *instance) {
    if (!cJSON_IsString(instance)) {
        return 1;
    }
    schema_pattern *pattern = schema_pattern_compile(value->valuestring);
    if (pattern == NULL) {
        unsupported(state, "the pattern", value->valuestring);
        return 0;
    }
    int found = schema_pattern_search(pattern, instance->valuestring, schema_json_string_length(instance));
    schema_pattern_release(pattern);
    return found;
}

static int check_length(const char *keyword, const cJSON *value, const cJSON *instance) {
    if (!cJSON_IsString(instance)) {
        return 1;
    }
    double length = (double)code_points(instance);
    return strcmp(keyword, "minLength") == 0 ? length >= value->valuedouble : length <= value->valuedouble;
}

static int check_unique(const cJSON *value, const cJSON *instance) {
    if (!cJSON_IsArray(instance) || !cJSON_IsTrue(value)) {
        return 1;
    }
    for (const cJSON *item = instance->child; item != NULL; item = item->next) {
        for (const cJSON *later = item->next; later != NULL; later = later->next) {
            if (schema_json_equal(item, later)) {
                return 0;
            }
        }
    }
    return 1;
}

static int check_items(validation *state, const cJSON *keyword, const cJSON *instance) {
    if (!cJSON_IsArray(instance)) {
        return 1;
    }
    double count = (double)cJSON_GetArraySize(instance);
    if (strcmp(keyword->string, "minItems") == 0) {
        return count >= keyword->valuedouble;
    }
    if (strcmp(keyword->string, "maxItems") == 0) {
        return count <= keyword->valuedouble;
    }
    if (strcmp(keyword->string, "uniqueItems") == 0) {
        return check_unique(keyword, instance);
    }
    int valid = 1;
    for (const cJSON *item = instance->child; item != NULL; item = item->next) {
        valid &= validate(state, keyword, item);
    }
    return valid;
}

static int check_required(const cJSON *value, const cJSON *instance) {
    int valid = 1;
    if (!cJSON_IsObject(instance)) {
        return 1;
    }
    for (const cJSON *name = value->child; name != NULL; name = name->next) {
        valid &= cJSON_GetObjectItemCaseSensitive(instance, name->valuestring) != NULL;
    }
    return valid;
}

static int check_members(validation *state, const cJSON *schema, const cJSON *keyword, const cJSON *instance) {
    const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    int is_properties = strcmp(keyword->string, "properties") == 0;
    int valid = 1;
    if (!cJSON_IsObject(instance)) {
        return 1;
    }
    for (const cJSON *member = instance->child; member != NULL; member = member->next) {
        const cJSON *named = properties == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(properties, member->string);
        if (is_properties && named != NULL) {
            valid &= validate(state, named, member);
        } else if (!is_properties && named == NULL) {
            valid &= validate(state, keyword, member);
        }
    }
    return valid;
}

static int check_names(validation *state, const cJSON *keyword, const cJSON *instance) {
    int valid = 1;
    if (!cJSON_IsObject(instance)) {
        return 1;
    }
    for (const cJSON *member = instance->child; member != NULL; member = member->next) {
        cJSON *name = cJSON_CreateString(member->string);
        valid &= name != NULL && validate(state, keyword, name);
        cJSON_Delete(name);
    }
    return valid;
}

static int check_combination(validation *state, const cJSON *keyword, const cJSON *instance) {
    int passed = 0;
    int total = 0;
    for (const cJSON *branch = keyword->child; branch != NULL; branch = branch->next) {
        passed += validate(state, branch, instance);
        total++;
    }
    if (strcmp(keyword->string, "allOf") == 0) {
        return passed == total;
    }
    return strcmp(keyword->string, "anyOf") == 0 ? passed > 0 : passed == 1;
}

static int check_condition(validation *state, const cJSON *schema, const cJSON *keyword, const cJSON *instance) {
    const cJSON *then = cJSON_GetObjectItemCaseSensitive(schema, "then");
    const cJSON *otherwise = cJSON_GetObjectItemCaseSensitive(schema, "else");
    const cJSON *chosen = validate(state, keyword, instance) ? then : otherwise;
    return chosen == NULL || validate(state, chosen, instance);
}

static int check_reference(validation *state, const cJSON *keyword, const cJSON *instance) {
    const cJSON *target = keyword->valuestring[0] == '#' ? schema_at(state->root, keyword->valuestring + 1) : NULL;
    if (target == NULL) {
        unsupported(state, "the reference", keyword->valuestring);
        return 0;
    }
    return validate(state, target, instance);
}

static int check_enum(const cJSON *keyword, const cJSON *instance) {
    for (const cJSON *allowed = keyword->child; allowed != NULL; allowed = allowed->next) {
        if (schema_json_equal(allowed, instance)) {
            return 1;
        }
    }
    return 0;
}

static int is_one_of(const char *keyword, const char *first, const char *second, const char *third) {
    return strcmp(keyword, first) == 0 || strcmp(keyword, second) == 0 || (third != NULL && strcmp(keyword, third) == 0);
}

static int check_keyword(validation *state, const cJSON *schema, const cJSON *keyword, const cJSON *instance) {
    const char *name = keyword->string;
    if (strcmp(name, "type") == 0 && cJSON_IsString(keyword)) {
        return is_type(instance, keyword->valuestring);
    }
    if (strcmp(name, "const") == 0) {
        return schema_json_equal(keyword, instance);
    }
    if (strcmp(name, "enum") == 0) {
        return check_enum(keyword, instance);
    }
    if (strcmp(name, "pattern") == 0) {
        return check_pattern(state, keyword, instance);
    }
    if (is_one_of(name, "minLength", "maxLength", NULL)) {
        return check_length(name, keyword, instance);
    }
    int is_items_schema = strcmp(name, "items") == 0 && !cJSON_IsArray(keyword);
    if (is_one_of(name, "minItems", "maxItems", "uniqueItems") || is_items_schema) {
        return check_items(state, keyword, instance);
    }
    if (strcmp(name, "required") == 0) {
        return check_required(keyword, instance);
    }
    if (is_one_of(name, "properties", "additionalProperties", NULL)) {
        return check_members(state, schema, keyword, instance);
    }
    if (strcmp(name, "propertyNames") == 0) {
        return check_names(state, keyword, instance);
    }
    if (strcmp(name, "not") == 0) {
        return !validate(state, keyword, instance);
    }
    if (is_one_of(name, "allOf", "anyOf", "oneOf")) {
        return check_combination(state, keyword, instance);
    }
    if (strcmp(name, "if") == 0) {
        return check_condition(state, schema, keyword, instance);
    }
    if (strcmp(name, "$ref") == 0 && cJSON_IsString(keyword)) {
        return check_reference(state, keyword, instance);
    }
    unsupported(state, "the keyword", name);
    return 0;
}

static int validate(validation *state, const cJSON *schema, const cJSON *instance) {
    int valid = 1;
    if (cJSON_IsBool(schema)) {
        return cJSON_IsTrue(schema);
    }
    if (!cJSON_IsObject(schema)) {
        unsupported(state, "a schema that is not an object at", schema->string == NULL ? "(root)" : schema->string);
        return 0;
    }
    for (const cJSON *keyword = schema->child; keyword != NULL; keyword = keyword->next) {
        if (!is_ignored(keyword->string)) {
            valid &= check_keyword(state, schema, keyword, instance);
        }
    }
    return valid;
}

schema_verdict schema_validate(const cJSON *root, const cJSON *schema, const cJSON *instance, char *problem,
                               size_t capacity) {
    validation state = {root, 0, problem, capacity};
    if (capacity > 0) {
        problem[0] = '\0';
    }
    int valid = validate(&state, schema, instance);
    if (state.unsupported) {
        return SCHEMA_UNSUPPORTED;
    }
    return valid ? SCHEMA_ACCEPTED : SCHEMA_REFUSED;
}

const cJSON *schema_at(const cJSON *root, const char *pointer) {
    const cJSON *current = root;
    char step[256];
    while (current != NULL && *pointer != '\0') {
        if (*pointer != '/') {
            return NULL;
        }
        pointer++;
        size_t length = strcspn(pointer, "/");
        if (length >= sizeof step) {
            return NULL;
        }
        memcpy(step, pointer, length);
        step[length] = '\0';
        pointer += length;
        current = cJSON_IsArray(current) ? cJSON_GetArrayItem(current, atoi(step))
                                         : cJSON_GetObjectItemCaseSensitive(current, step);
    }
    return current;
}
