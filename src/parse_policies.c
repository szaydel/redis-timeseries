/*
 *copyright redis ltd. 2017 - present
 *licensed under your choice of the redis source available license 2.0 (rsalv2) or
 *the server side public license v1 (ssplv1).
 */
#include "parse_policies.h"

#include "compaction.h"
#include "consts.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "rmutil/util.h"
#include <rmutil/alloc.h>

#define SINGLE_RULE_ITEM_STRING_LENGTH 32

static const timestamp_t lookup_intervals[] = { ['m'] = 1,
                                                ['s'] = 1000,
                                                ['M'] = 1000 * 60,
                                                ['h'] = 1000 * 60 * 60,
                                                ['d'] = 1000 * 60 * 60 * 24 };

static int parse_string_to_millisecs(const char *timeStr, timestamp_t *out, bool canBeZero) {
    char should_be_empty;
    unsigned char interval_type;
    timestamp_t timeSize;
    int ret;
    ret = sscanf(timeStr, "%" SCNu64 "%c%c", &timeSize, &interval_type, &should_be_empty);
    bool valid_state = (ret == 2) || (ret == 1 && timeSize == 0);
    if (!valid_state) {
        return FALSE;
    }

    if (canBeZero && timeSize == 0) {
        *out = 0;
        return TRUE;
    }

    timestamp_t interval_in_millisecs = lookup_intervals[interval_type];
    if (interval_in_millisecs == 0) {
        return FALSE;
    }
    *out = interval_in_millisecs * timeSize;
    return TRUE;
}

static int parse_interval_policy(char *policy, SimpleCompactionRule *rule) {
    char *token;
    char *token_iter_ptr;
    char agg_type[20];
    rule->timestampAlignment = 0; // the default alignment is 0

    token = strtok_r(policy, ":", &token_iter_ptr);
    int i = 0;
    while (token) {
        if (i == 0) { // first param its the aggregation type
            strcpy(agg_type, token);
        } else if (i == 1) { // the 2nd param is the bucket
            if (parse_string_to_millisecs(token, &rule->bucketDuration, false) == FALSE) {
                return FALSE;
            }
        } else if (i == 2) {
            if (parse_string_to_millisecs(token, &rule->retentionSizeMillisec, true) == FALSE) {
                return FALSE;
            }
        } else if (i == 3) {
            if (parse_string_to_millisecs(token, &rule->timestampAlignment, false) == FALSE) {
                return FALSE;
            }
        } else {
            return FALSE;
        }

        i++;
        token = strtok_r(NULL, ":", &token_iter_ptr);
    }
    if (i < 3) {
        return FALSE;
    }
    int agg_type_index = StringAggTypeToEnum(agg_type);
    if (agg_type_index == TS_AGG_INVALID) {
        return FALSE;
    }
    rule->aggType = agg_type_index;

    return TRUE;
}

static size_t count_char_in_str(const char *string, size_t len, char lookup) {
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (string[i] == lookup) {
            count++;
        }
    }
    return count;
}

// parse compaction policies in the following format: "max:1m;min:10s;avg:2h;avg:3d"
// the format is AGGREGATION_FUNCTION:\d[s|m|h|d];
int ParseCompactionPolicy(const char *policy_string,
                          SimpleCompactionRule **parsed_rules_out,
                          uint64_t *rules_count) {
    char *token;
    char *token_iter_ptr;
    size_t len = strlen(policy_string);
    char *rest = malloc(len + 1);
    memcpy(rest, policy_string, len + 1);
    *parsed_rules_out = NULL;
    *rules_count = 0;

    // the ';' is a separator so we need to add +1 for the policy count
    uint64_t policies_count = count_char_in_str(policy_string, len, ';') + 1;
    *parsed_rules_out = malloc(sizeof(SimpleCompactionRule) * policies_count);
    SimpleCompactionRule *parsed_rules_runner = *parsed_rules_out;

    token = strtok_r(rest, ";", &token_iter_ptr);
    int success = TRUE;
    while (token != NULL) {
        int result = parse_interval_policy(token, parsed_rules_runner);
        if (result == FALSE) {
            success = FALSE;
            break;
        }
        token = strtok_r(NULL, ";", &token_iter_ptr);
        parsed_rules_runner++;
        *rules_count = *rules_count + 1;
    }

    free(rest);
    if (success == FALSE) {
        // all or nothing, don't allow partial parsing
        *rules_count = 0;
        if (*parsed_rules_out) {
            free(*parsed_rules_out);
            *parsed_rules_out = NULL;
        }
    }
    return success;
}

// Helper function to convert milliseconds to a time string
static inline void MillisecondsToTimeString(uint64_t millis, char *out, const size_t outLength) {
    if (millis % (1000 * 60 * 60 * 24) == 0) {
        snprintf(out, outLength, "%" PRIu64 "d", millis / (1000 * 60 * 60 * 24));
    } else if (millis % (1000 * 60 * 60) == 0) {
        snprintf(out, outLength, "%" PRIu64 "h", millis / (1000 * 60 * 60));
    } else if (millis % (1000 * 60) == 0) {
        snprintf(out, outLength, "%" PRIu64 "M", millis / (1000 * 60));
    } else if (millis % 1000 == 0) {
        snprintf(out, outLength, "%" PRIu64 "s", millis / 1000);
    } else {
        snprintf(out, outLength, "%" PRIu64 "m", millis);
    }
}

char *CompactionRulesToString(const SimpleCompactionRule *compactionRules,
                              const uint64_t compactionRulesCount) {
    if (compactionRules == NULL || compactionRulesCount == 0) {
        return NULL;
    }

    // Estimate a sufficiently large buffer size
    size_t buffer_size = 256 * compactionRulesCount;
    char *result = malloc(buffer_size);
    if (!result) {
        return NULL;
    }
    result[0] = '\0'; // Initialize the string

    for (uint64_t i = 0; i < compactionRulesCount; ++i) {
        const SimpleCompactionRule *rule = &compactionRules[i];

        // Convert bucket duration and retention size to strings
        char bucket_duration[SINGLE_RULE_ITEM_STRING_LENGTH] = { 0 };
        char retention[SINGLE_RULE_ITEM_STRING_LENGTH] = { 0 };
        char alignment[SINGLE_RULE_ITEM_STRING_LENGTH] = { 0 };

        MillisecondsToTimeString(
            rule->bucketDuration, bucket_duration, SINGLE_RULE_ITEM_STRING_LENGTH);
        MillisecondsToTimeString(
            rule->retentionSizeMillisec, retention, SINGLE_RULE_ITEM_STRING_LENGTH);
        if (rule->timestampAlignment > 0) {
            MillisecondsToTimeString(
                rule->timestampAlignment, alignment, SINGLE_RULE_ITEM_STRING_LENGTH);
        }

        // Get aggregation type as string
        const char *aggTypeStr = AggTypeEnumToStringLowerCase(rule->aggType);
        if (!aggTypeStr) {
            free(result);
            return NULL; // Invalid aggregation type
        }

        // Append the rule to the result string
        if (rule->timestampAlignment > 0) {
            snprintf(result + strlen(result),
                     buffer_size - strlen(result),
                     "%s:%s:%s:%s;",
                     aggTypeStr,
                     bucket_duration,
                     retention,
                     alignment);
        } else {
            snprintf(result + strlen(result),
                     buffer_size - strlen(result),
                     "%s:%s:%s;",
                     aggTypeStr,
                     bucket_duration,
                     retention);
        }
    }

    // Remove the trailing semicolon
    size_t len = strlen(result);
    if (len > 0 && result[len - 1] == ';') {
        result[len - 1] = '\0';
    }

    return result;
}
