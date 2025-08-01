/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#define REDISMODULE_MAIN

#include "module.h"

#include "compaction.h"
#include "common.h"
#include "config.h"
#include "indexer.h"
#include "libmr_commands.h"
#include "libmr_integration.h"
#include "query_language.h"
#include "rdb.h"
#include "reply.h"
#include "resultset.h"
#include "short_read.h"
#include "tsdb.h"
#include "version.h"

#include "fast_double_parser_c/fast_double_parser_c.h"
#include "LibMR/src/cluster.h"
#include "LibMR/src/mr.h"
#include "RedisModulesSDK/redismodule.h"
#include "rmutil/alloc.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <math.h>

#ifndef REDISTIMESERIES_GIT_SHA
#define REDISTIMESERIES_GIT_SHA "unknown"
#endif

#define TIMESERIES_MODULE_ACL_CATEGORY_NAME "timeseries"

#define SetCommandAcls(ctx, cmd, acls)                                                             \
    {                                                                                              \
        if (RedisModule_GetCommand && RedisModule_AddACLCategory &&                                \
            RedisModule_SetCommandACLCategories) {                                                 \
            RedisModuleCommand *command = RedisModule_GetCommand(ctx, cmd);                        \
            if (command == NULL) {                                                                 \
                return REDISMODULE_ERR;                                                            \
            }                                                                                      \
                                                                                                   \
            const char *categories = acls " " TIMESERIES_MODULE_ACL_CATEGORY_NAME;                 \
            const int ret = RedisModule_SetCommandACLCategories(command, categories);              \
                                                                                                   \
            if (ret != REDISMODULE_OK) {                                                           \
                return REDISMODULE_ERR;                                                            \
            }                                                                                      \
        }                                                                                          \
    }

#define RegisterCommandWithModesAndAcls(ctx, cmd, f, mode, acls)                                   \
    __rmutil_register_cmd(ctx, cmd, f, mode);                                                      \
    SetCommandAcls(ctx, cmd, acls);

static bool LoadConfiguration(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    InitConfig();

    const bool isConfigApiSupported = RTS_RedisSupportsModuleConfigApi();

    if (ReadDeprecatedLoadTimeConfig(ctx, argv, argc, isConfigApiSupported) == TSDB_ERROR) {
        RedisModule_Log(
            ctx,
            "warning",
            "Failed to parse the deprecated RedisTimeSeries configurations, aborting...");

        FreeConfig();

        return false;
    }

    if (!isConfigApiSupported) {
        // Nothing else to do here.
        return true;
    }

    if (!RegisterConfigurationOptions(ctx)) {
        RedisModule_Log(
            ctx, "warning", "Failed to register the RedisTimeSeries configurations, aborting...");

        FreeConfig();

        return false;
    }

    if (RedisModule_LoadConfigs(ctx) != REDISMODULE_OK) {
        RedisModule_Log(
            ctx, "warning", "Failed to load the RedisTimeSeries configurations, aborting...");

        FreeConfig();

        return false;
    }

    return true;
}

RedisModuleType *SeriesType;
RedisModuleCtx *rts_staticCtx; // global redis ctx
bool isTrimming = false;

int TSDB_info(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2 || argc > 3) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    const GetSeriesResult status =
        GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ, GetSeriesFlags_DeleteReferences);
    if (status != GetSeriesResult_Success) {
        return REDISMODULE_ERR;
    }

    bool reply_map = _ReplyMap(ctx);

    int is_debug = RMUtil_ArgExists("DEBUG", argv, argc, 1);
    if (is_debug) {
        RedisModule_ReplyWithMapOrArray(ctx, 16 * 2, true);
    } else {
        RedisModule_ReplyWithMapOrArray(ctx, 14 * 2, true);
    }

    long long skippedSamples;
    long long firstTimestamp = getFirstValidTimestamp(series, &skippedSamples);

    RedisModule_ReplyWithSimpleString(ctx, "totalSamples");
    RedisModule_ReplyWithLongLong(ctx, SeriesGetNumSamples(series) - skippedSamples);
    RedisModule_ReplyWithSimpleString(ctx, "memoryUsage");
    RedisModule_ReplyWithLongLong(ctx, SeriesMemUsage(series));
    RedisModule_ReplyWithSimpleString(ctx, "firstTimestamp");
    RedisModule_ReplyWithLongLong(ctx, firstTimestamp);
    RedisModule_ReplyWithSimpleString(ctx, "lastTimestamp");
    RedisModule_ReplyWithLongLong(ctx, series->lastTimestamp);
    RedisModule_ReplyWithSimpleString(ctx, "retentionTime");
    RedisModule_ReplyWithLongLong(ctx, series->retentionTime);
    RedisModule_ReplyWithSimpleString(ctx, "chunkCount");
    RedisModule_ReplyWithLongLong(ctx, RedisModule_DictSize(series->chunks));
    RedisModule_ReplyWithSimpleString(ctx, "chunkSize");
    RedisModule_ReplyWithLongLong(ctx, series->chunkSizeBytes);
    RedisModule_ReplyWithSimpleString(ctx, "chunkType");
    RedisModule_ReplyWithSimpleString(ctx, ChunkTypeToString(series->options));
    RedisModule_ReplyWithSimpleString(ctx, "duplicatePolicy");
    if (series->duplicatePolicy != DP_NONE) {
        RedisModule_ReplyWithSimpleString(ctx, DuplicatePolicyToString(series->duplicatePolicy));
    } else {
        RedisModule_ReplyWithNull(ctx);
    }

    RedisModule_ReplyWithSimpleString(ctx, "labels");
    ReplyWithSeriesLabels(ctx, series);

    RedisModule_ReplyWithSimpleString(ctx, "sourceKey");
    if (series->srcKey == NULL) {
        RedisModule_ReplyWithNull(ctx);
    } else {
        RedisModule_ReplyWithString(ctx, series->srcKey);
    }

    RedisModule_ReplyWithSimpleString(ctx, "rules");
    RedisModule_ReplyWithMapOrArray(ctx, REDISMODULE_POSTPONED_LEN, false);
    CompactionRule *rule = series->rules;
    int ruleCount = 0;
    while (rule != NULL) {
        if (!reply_map) {
            RedisModule_ReplyWithArray(ctx, 4);
        }
        RedisModule_ReplyWithString(ctx, rule->destKey);
        if (reply_map) {
            RedisModule_ReplyWithArray(ctx, 3);
        }
        RedisModule_ReplyWithLongLong(ctx, rule->bucketDuration);
        RedisModule_ReplyWithSimpleString(ctx, AggTypeEnumToString(rule->aggType));
        RedisModule_ReplyWithLongLong(ctx, rule->timestampAlignment);

        rule = rule->nextRule;
        ruleCount++;
    }
    RedisModule_ReplySetMapOrArrayLength(ctx, ruleCount, false);

    RedisModule_ReplyWithSimpleString(ctx, "ignoreMaxTimeDiff");
    RedisModule_ReplyWithLongLong(ctx, series->ignoreMaxTimeDiff);
    RedisModule_ReplyWithSimpleString(ctx, "ignoreMaxValDiff");
    RedisModule_ReplyWithDouble(ctx, series->ignoreMaxValDiff);

    if (is_debug) {
        RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, ">", "", 0);
        Chunk_t *chunk = NULL;
        int chunkCount = 0;
        RedisModule_ReplyWithSimpleString(ctx, "keySelfName");
        RedisModule_ReplyWithString(ctx, series->keyName);
        RedisModule_ReplyWithSimpleString(ctx, "Chunks");
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
        while (RedisModule_DictNextC(iter, NULL, (void *)&chunk)) {
            uint64_t numOfSamples = series->funcs->GetNumOfSample(chunk);
            size_t chunkSize = series->funcs->GetChunkSize(chunk, false);
            if (!reply_map) {
                RedisModule_ReplyWithArray(ctx, 5 * 2);
            } else {
                RedisModule_ReplyWithMap(ctx, 5);
            }

            RedisModule_ReplyWithSimpleString(ctx, "startTimestamp");
            RedisModule_ReplyWithLongLong(
                ctx, numOfSamples == 0 ? -1 : series->funcs->GetFirstTimestamp(chunk));
            RedisModule_ReplyWithSimpleString(ctx, "endTimestamp");
            RedisModule_ReplyWithLongLong(
                ctx, numOfSamples == 0 ? -1 : series->funcs->GetLastTimestamp(chunk));
            RedisModule_ReplyWithSimpleString(ctx, "samples");
            RedisModule_ReplyWithLongLong(ctx, numOfSamples);
            RedisModule_ReplyWithSimpleString(ctx, "size");
            RedisModule_ReplyWithLongLong(ctx, chunkSize);
            RedisModule_ReplyWithSimpleString(ctx, "bytesPerSample");
            RedisModule_ReplyWithDouble(
                ctx, (numOfSamples == 0) ? (float)0 : (float)chunkSize / numOfSamples);
            chunkCount++;
        }
        RedisModule_DictIteratorStop(iter);
        RedisModule_ReplySetArrayLength(ctx, chunkCount);
    }
    RedisModule_CloseKey(key);

    return REDISMODULE_OK;
}

void _TSDB_queryindex_impl(RedisModuleCtx *ctx, QueryPredicateList *queries) {
    RedisModuleDict *result = QueryIndex(ctx, queries->list, queries->count, NULL);

    RedisModule_ReplyWithSetOrArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModule_ReplyWithStringBuffer(ctx, currentKey, currentKeyLen);
        replylen++;
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_ReplySetSetOrArrayLength(ctx, replylen);
}

int TSDB_queryindex(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    int query_count = argc - 1;

    int response = 0;
    QueryPredicateList *queries = parseLabelListFromArgs(ctx, argv, 1, query_count, &response);
    if (response == TSDB_ERROR) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
    }

    if (CountPredicateType(queries, EQ) + CountPredicateType(queries, LIST_MATCH) == 0) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
    }

    if (IsMRCluster()) {
        int ctxFlags = RedisModule_GetContextFlags(ctx);

        if (ctxFlags & (REDISMODULE_CTX_FLAGS_LUA | REDISMODULE_CTX_FLAGS_MULTI |
                        REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
            RedisModule_ReplyWithError(ctx,
                                       "Can not run multi sharded command inside a multi exec, "
                                       "lua, or when blocking is not allowed");
            return REDISMODULE_OK;
        }
        TSDB_queryindex_RG(ctx, queries);
        QueryPredicateList_Free(queries);
    } else {
        _TSDB_queryindex_impl(ctx, queries);
        QueryPredicateList_Free(queries);
    }

    return REDISMODULE_OK;
}

// multi-series groupby logic
static int replyGroupedMultiRange(RedisModuleCtx *ctx,
                                  TS_ResultSet *resultset,
                                  RedisModuleDict *result,
                                  const MRangeArgs *args) {
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey = NULL;
    size_t currentKeyLen;
    Series *series = NULL;
    int exitStatus = REDISMODULE_OK;
    const GetSeriesFlags flags = GetSeriesFlags_SilentOperation | GetSeriesFlags_CheckForAcls;

    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const GetSeriesResult status =
            GetSeries(ctx,
                      RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                      &key,
                      &series,
                      REDISMODULE_READ,
                      flags);

        switch (status) {
            case GetSeriesResult_Success:
                RedisModule_CloseKey(key);

                break;
            case GetSeriesResult_GenericError:
                RedisModule_Log(ctx,
                                "warning",
                                "couldn't open key or key is not a Timeseries. key=%s",
                                currentKey);

                continue;
            case GetSeriesResult_PermissionError:
                RedisModule_Log(ctx,
                                "warning",
                                "The user lacks the required permissions for the key=%s, stopping.",
                                currentKey);
                exitStatus = REDISMODULE_ERR;

                goto exit;
        }
    }

    RedisModule_DictIteratorStop(iter);
    iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);

    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const GetSeriesResult status =
            GetSeries(ctx,
                      RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                      &key,
                      &series,
                      REDISMODULE_READ,
                      flags);
        if (status != GetSeriesResult_Success) {
            // The iterator may have been invalidated, stop and restart from after the current
            // key.
            RedisModule_DictIteratorStop(iter);
            iter = RedisModule_DictIteratorStartC(result, ">", currentKey, currentKeyLen);
            continue;
        }

        ResultSet_AddSerie(resultset, series, RedisModule_StringPtrLen(series->keyName, NULL));
        RedisModule_CloseKey(key);
    }
    RedisModule_DictIteratorStop(iter);

    // todo: this is duplicated in resultset.c
    // Apply the reducer
    ResultSet_ApplyReducer(ctx, resultset, &args->rangeArgs, &args->gropuByReducerArgs);

    // Do not apply the aggregation on the resultset, do apply max results on the final result
    RangeArgs minimizedArgs = args->rangeArgs;
    minimizedArgs.startTimestamp = 0;
    minimizedArgs.endTimestamp = UINT64_MAX;
    minimizedArgs.aggregationArgs.aggregationClass = NULL;
    minimizedArgs.aggregationArgs.timeDelta = 0;
    minimizedArgs.filterByTSArgs.hasValue = false;
    minimizedArgs.filterByValueArgs.hasValue = false;
    minimizedArgs.latest = false;

    replyResultSet(ctx,
                   resultset,
                   args->withLabels,
                   (RedisModuleString **)args->limitLabels,
                   args->numLimitLabels,
                   &minimizedArgs,
                   args->reverse);
exit:
    ResultSet_Free(resultset);
    return exitStatus;
}

// Previous multirange reply logic ( unchanged )
static int replyUngroupedMultiRange(RedisModuleCtx *ctx,
                                    RedisModuleDict *result,
                                    const MRangeArgs *args) {
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    RedisModuleString *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    Series *series;
    int exitStatus = REDISMODULE_OK;
    const GetSeriesFlags flags = GetSeriesFlags_SilentOperation | GetSeriesFlags_CheckForAcls;

    while ((currentKey = RedisModule_DictNext(ctx, iter, NULL)) != NULL) {
        RedisModuleKey *key;
        const GetSeriesResult status =
            GetSeries(ctx, currentKey, &key, &series, REDISMODULE_READ, flags);

        switch (status) {
            case GetSeriesResult_Success:
                RedisModule_CloseKey(key);
                RedisModule_FreeString(ctx, currentKey);

                break;
            case GetSeriesResult_GenericError:
                RedisModule_Log(ctx,
                                "warning",
                                "couldn't open key or key is not a Timeseries. key=%s",
                                RedisModule_StringPtrLen(currentKey, NULL));
                RedisModule_FreeString(ctx, currentKey);

                break;
            case GetSeriesResult_PermissionError:
                RedisModule_Log(ctx,
                                "warning",
                                "The user lacks the required permissions for the key=%s, stopping.",
                                RedisModule_StringPtrLen(currentKey, NULL));
                RedisModule_FreeString(ctx, currentKey);
                exitStatus = REDISMODULE_ERR;

                goto exit;
        }
    }

    RedisModule_DictIteratorStop(iter);
    iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    RedisModule_ReplyWithMapOrArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN, false);
    while ((currentKey = RedisModule_DictNext(ctx, iter, NULL)) != NULL) {
        RedisModuleKey *key;
        const GetSeriesResult status =
            GetSeries(ctx, currentKey, &key, &series, REDISMODULE_READ, flags);

        if (status != GetSeriesResult_Success) {
            // The iterator may have been invalidated, stop and restart from after the current key.
            RedisModule_DictIteratorStop(iter);
            iter = RedisModule_DictIteratorStart(result, ">", currentKey);
            RedisModule_FreeString(ctx, currentKey);
            continue;
        }

        ReplySeriesArrayPos(ctx,
                            series,
                            args->withLabels,
                            (RedisModuleString **)args->limitLabels,
                            args->numLimitLabels,
                            &args->rangeArgs,
                            args->reverse,
                            false);
        replylen++;
        RedisModule_CloseKey(key);
        RedisModule_FreeString(ctx, currentKey);
    }

exit:
    RedisModule_DictIteratorStop(iter);

    if (exitStatus == REDISMODULE_OK) {
        RedisModule_ReplySetMapOrArrayLength(ctx, replylen, false);
    }

    return exitStatus;
}

int TSDB_generic_mrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool rev) {
    RedisModule_AutoMemory(ctx);

    MRangeArgs args;
    if (parseMRangeCommand(ctx, argv, argc, &args) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    args.reverse = rev;

    bool hasPermissionError = false;
    RedisModuleDict *resultSeries = QueryIndex(
        ctx, args.queryPredicates->list, args.queryPredicates->count, &hasPermissionError);

    if (hasPermissionError) {
        MRangeArgs_Free(&args);
        RTS_ReplyKeyPermissionsError(ctx);
        return REDISMODULE_ERR;
    }

    int result = REDISMODULE_OK;
    if (args.groupByLabel) {
        TS_ResultSet *resultset = ResultSet_Create();
        ResultSet_GroupbyLabel(resultset, args.groupByLabel);

        result = replyGroupedMultiRange(ctx, resultset, resultSeries, &args);
    } else {
        result = replyUngroupedMultiRange(ctx, resultSeries, &args);
    }

    MRangeArgs_Free(&args);
    return result;
}

int TSDB_mrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (IsMRCluster()) {
        if (!IsCurrentUserAllowedToReadAllTheKeys(ctx)) {
            return RTS_ReplyKeyPermissionsError(ctx);
        }

        int ctxFlags = RedisModule_GetContextFlags(ctx);

        if (ctxFlags & (REDISMODULE_CTX_FLAGS_LUA | REDISMODULE_CTX_FLAGS_MULTI |
                        REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
            RedisModule_ReplyWithError(ctx,
                                       "Can not run multi sharded command inside a multi exec, "
                                       "lua, or when blocking is not allowed");
            return REDISMODULE_OK;
        }
        return TSDB_mrange_RG(ctx, argv, argc, false);
    }

    return TSDB_generic_mrange(ctx, argv, argc, false);
}

int TSDB_mrevrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (IsMRCluster()) {
        if (!IsCurrentUserAllowedToReadAllTheKeys(ctx)) {
            return RTS_ReplyKeyPermissionsError(ctx);
        }

        int ctxFlags = RedisModule_GetContextFlags(ctx);

        if (ctxFlags & (REDISMODULE_CTX_FLAGS_LUA | REDISMODULE_CTX_FLAGS_MULTI |
                        REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
            RedisModule_ReplyWithError(ctx,
                                       "Can not run multi sharded command inside a multi exec, "
                                       "lua, or when blocking is not allowed");
            return REDISMODULE_OK;
        }
        return TSDB_mrange_RG(ctx, argv, argc, true);
    }
    return TSDB_generic_mrange(ctx, argv, argc, true);
}

int TSDB_generic_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool rev) {
    if (argc < 4) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    const GetSeriesResult status =
        GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ, GetSeriesFlags_CheckForAcls);
    if (status != GetSeriesResult_Success) {
        return REDISMODULE_ERR;
    }

    RangeArgs rangeArgs = { 0 };
    if (parseRangeArguments(ctx, 2, argv, argc, &rangeArgs) != REDISMODULE_OK) {
        goto _out;
    }

    ReplySeriesRange(ctx, series, &rangeArgs, rev);

_out:
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int TSDB_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_range(ctx, argv, argc, false);
}

int TSDB_revrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_range(ctx, argv, argc, true);
}

static int internalAdd(RedisModuleCtx *ctx,
                       Series *series,
                       api_timestamp_t timestamp,
                       double value,
                       DuplicatePolicy dp_override,
                       bool should_reply);

static void handleCompaction(RedisModuleCtx *ctx,
                             Series *series,
                             CompactionRule *rule,
                             api_timestamp_t timestamp,
                             double value) {
    timestamp_t currentTimestamp =
        CalcBucketStart(timestamp, rule->bucketDuration, rule->timestampAlignment);
    timestamp_t currentTimestampNormalized = BucketStartNormalize(currentTimestamp);

    if (rule->startCurrentTimeBucket == -1LL) {
        // first sample, lets init the startCurrentTimeBucket
        rule->startCurrentTimeBucket = currentTimestampNormalized;

        if (rule->aggClass->type == TS_AGG_TWA) {
            rule->aggClass->addBucketParams(rule->aggContext,
                                            currentTimestampNormalized,
                                            currentTimestamp + rule->bucketDuration);
        }
    }

    if (currentTimestampNormalized > rule->startCurrentTimeBucket) {
        Series *destSeries;
        RedisModuleKey *key;
        const GetSeriesFlags flags = GetSeriesFlags_SilentOperation | GetSeriesFlags_CheckForAcls;
        const GetSeriesResult status = GetSeries(
            ctx, rule->destKey, &key, &destSeries, REDISMODULE_READ | REDISMODULE_WRITE, flags);
        if (status != GetSeriesResult_Success) {
            // key doesn't exist anymore or some other error occurred,
            // and we don't do anything
            return;
        }

        if (rule->aggClass->type == TS_AGG_TWA) {
            rule->aggClass->addNextBucketFirstSample(rule->aggContext, value, timestamp);
        }

        double aggVal;
        if (rule->aggClass->finalize(rule->aggContext, &aggVal) == TSDB_OK) {
            internalAdd(ctx, destSeries, rule->startCurrentTimeBucket, aggVal, DP_LAST, false);
            RedisModule_NotifyKeyspaceEvent(
                ctx, REDISMODULE_NOTIFY_MODULE, "ts.add:dest", rule->destKey);
        }
        Sample last_sample;
        if (rule->aggClass->type == TS_AGG_TWA) {
            rule->aggClass->getLastSample(rule->aggContext, &last_sample);
        }
        rule->aggClass->resetContext(rule->aggContext);
        if (rule->aggClass->type == TS_AGG_TWA) {
            rule->aggClass->addBucketParams(rule->aggContext,
                                            currentTimestampNormalized,
                                            currentTimestamp + rule->bucketDuration);
        }

        if (rule->aggClass->type == TS_AGG_TWA) {
            rule->aggClass->addPrevBucketLastSample(
                rule->aggContext, last_sample.value, last_sample.timestamp);
        }
        rule->startCurrentTimeBucket = currentTimestampNormalized;
        RedisModule_CloseKey(key);
    }
    rule->aggClass->appendValue(rule->aggContext, value, timestamp);
}

static inline bool filter_close_samples(DuplicatePolicy dp_policy,
                                        const Series *series,
                                        api_timestamp_t timestamp,
                                        double value) {
    return dp_policy == DP_LAST && series->totalSamples != 0 &&
           timestamp >= series->lastTimestamp &&
           timestamp - series->lastTimestamp <= series->ignoreMaxTimeDiff &&
           fabs(value - series->lastValue) <= series->ignoreMaxValDiff;
}

static int internalAdd(RedisModuleCtx *ctx,
                       Series *series,
                       api_timestamp_t timestamp,
                       double value,
                       DuplicatePolicy dp_override,
                       bool should_reply) {
    const timestamp_t lastTS = series->lastTimestamp;
    const uint64_t retention = series->retentionTime;
    // ensure inside retention period.
    if (retention && timestamp < lastTS && retention < lastTS - timestamp) {
        RTS_ReplyGeneralError(ctx, "TSDB: Timestamp is older than retention");
        return REDISMODULE_ERR;
    }

    // Use module level configuration if key level configuration doesn't exist
    const DuplicatePolicy dp_policy =
        dp_override ?: series->duplicatePolicy ?: TSGlobalConfig.duplicatePolicy;

    // Insert filter for close samples. If configured, it's used to ignore last measurement if its
    // value is negligible compared to the last sample.
    if (filter_close_samples(dp_policy, series, timestamp, value)) {
        RedisModule_ReplyWithLongLong(ctx, series->lastTimestamp);
        return REDISMODULE_ERR;
    }

    if (timestamp <= series->lastTimestamp && series->totalSamples != 0) {
        if (SeriesUpsertSample(series, timestamp, value, dp_policy) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx,
                                  "TSDB: Error at upsert, update is not supported when "
                                  "DUPLICATE_POLICY is set to BLOCK mode");
            return REDISMODULE_ERR;
        }
    } else {
        if (SeriesAddSample(series, timestamp, value) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Error at add");
            return REDISMODULE_ERR;
        }
        // handle compaction rules
        if (series->rules) {
            const GetSeriesFlags flags =
                GetSeriesFlags_SilentOperation | GetSeriesFlags_CheckForAcls;
            deleteReferenceToDeletedSeries(ctx, series, flags);
        }

        for (CompactionRule *rule = series->rules; rule != NULL; rule = rule->nextRule) {
            handleCompaction(ctx, series, rule, timestamp, value);
        }
    }
    if (should_reply) {
        RedisModule_ReplyWithLongLong(ctx, timestamp);
    }
    return REDISMODULE_OK;
}

static inline double parse_double(const RedisModuleString *valueStr) {
    size_t len;
    char const *const valueCStr = RedisModule_StringPtrLen(valueStr, &len);
    double value;
    char const *const endptr = fast_double_parser_c_parse_number(valueCStr, &value);
    return endptr && endptr - valueCStr == len ? value : NAN;
}

static inline int add(RedisModuleCtx *ctx,
                      RedisModuleString *keyName,
                      const RedisModuleString *timestampStr,
                      const RedisModuleString *valueStr,
                      RedisModuleString **argv,
                      int argc) {
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE);

    const double value = parse_double(valueStr);
    if (isnan(value)) {
        RTS_ReplyGeneralError(ctx, "TSDB: invalid value");
        return REDISMODULE_ERR;
    }

    long long timestampValue;
    if (RedisModule_StringToLongLong(timestampStr, &timestampValue) != REDISMODULE_OK) {
        RTS_ReplyGeneralError(ctx, "TSDB: invalid timestamp");
        return REDISMODULE_ERR;
    }
    if (timestampValue < 0) {
        RTS_ReplyGeneralError(ctx, "TSDB: invalid timestamp, must be a nonnegative integer");
        return REDISMODULE_ERR;
    }
    const api_timestamp_t timestamp = (api_timestamp_t)timestampValue;

    Series *series = NULL;
    DuplicatePolicy dp = DP_NONE;

    if (argv != NULL && RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        // the key doesn't exist, lets check we have enough information to create one
        CreateCtx cCtx = { 0 };
        if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }

        CreateTsKey(ctx, keyName, &cCtx, &series, &key);
        SeriesCreateRulesFromGlobalConfig(ctx, keyName, series, cCtx.labels, cCtx.labelsCount);
    } else if (RedisModule_ModuleTypeGetType(key) != SeriesType) {
        RTS_ReplyGeneralError(ctx, "TSDB: the key is not a TSDB key");
        return REDISMODULE_ERR;
    } else {
        series = RedisModule_ModuleTypeGetValue(key);
        //  override key and database configuration for DUPLICATE_POLICY
        if (argv != NULL &&
            ParseDuplicatePolicy(ctx, argv, argc, TS_ADD_DUPLICATE_POLICY_ARG, &dp, NULL) !=
                TSDB_OK) {
            return REDISMODULE_ERR;
        }
    }
    const int rv = internalAdd(ctx, series, timestamp, value, dp, true);
    RedisModule_CloseKey(key);
    return rv;
}

static inline RedisModuleString *getCurrentTime(RedisModuleCtx *ctx) {
    return RedisModule_CreateStringPrintf(ctx, "%llu", RedisModule_Milliseconds());
}

int TSDB_madd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 4 || (argc - 1) % 3 != 0) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *curTimeStr = NULL;

    RedisModule_ReplyWithArray(ctx, (argc - 1) / 3);
    const RedisModuleString **replArgv = malloc((argc - 1) * sizeof *replArgv);
    const RedisModuleString **offset = replArgv;
    for (int i = 1; i < argc; i += 3) {
        RedisModuleString *keyName = argv[i];
        const RedisModuleString *timestampStr = argv[i + 1];
        const RedisModuleString *valueStr = argv[i + 2];

        if (stringEqualsC(timestampStr, "*")) {
            // if timestamp is "*", take current time (automatic timestamp)
            if (!curTimeStr) {
                curTimeStr = getCurrentTime(ctx);
            }
            timestampStr = curTimeStr;
        }

        if (add(ctx, keyName, timestampStr, valueStr, NULL, -1) == REDISMODULE_OK) {
            *offset++ = keyName;
            *offset++ = timestampStr;
            *offset++ = valueStr;
        }
    }
    const size_t replArgc = offset - replArgv;

    if (replArgc > 0) {
        // we want to replicate only successful sample inserts to avoid errors on the replica, when
        // this errors occurs, redis will CRITICAL error to its log and potentially fill up the disk
        // depending on the actual traffic.
        RedisModule_Replicate(ctx, "TS.MADD", "v", replArgv, replArgc);
    }
    free(replArgv);

    for (int i = 1; i < argc; i += 3) {
        RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.add", argv[i]);
    }

    return REDISMODULE_OK;
}

int TSDB_add(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 4) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *keyName = argv[1];
    const RedisModuleString *timestampStr = argv[2];
    const RedisModuleString *valueStr = argv[3];

    if (stringEqualsC(timestampStr, "*")) {
        // if timestamp is "*", take current time (automatic timestamp)
        timestampStr = getCurrentTime(ctx);
    }

    const int result = add(ctx, keyName, timestampStr, valueStr, argv, argc);
    if (result == REDISMODULE_OK) {
        const size_t replArgc = argc - 1;
        const RedisModuleString **replArgv = malloc(replArgc * sizeof *replArgv);
        for (int i = 0; i < replArgc; i++) { // skip the command name
            replArgv[i] = argv[i + 1];
        }
        replArgv[1] = timestampStr; // In case the timestamp was "*"
        RedisModule_Replicate(ctx, "TS.ADD", "v", replArgv, replArgc);
        free(replArgv);
    }

    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.add", keyName);

    return result;
}

int CreateTsKey(RedisModuleCtx *ctx,
                RedisModuleString *keyName,
                const CreateCtx *cCtx,
                Series **series,
                RedisModuleKey **key) {
    if (*key == NULL) {
        *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE);
    }

    RedisModule_RetainString(ctx, keyName);
    *series = NewSeries(keyName, cCtx);
    if (RedisModule_ModuleTypeSetValue(*key, SeriesType, *series) == REDISMODULE_ERR) {
        return TSDB_ERROR;
    }

    IndexMetric(keyName, (*series)->labels, (*series)->labelsCount);

    return TSDB_OK;
}

int TSDB_create(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleString *keyName = argv[1];
    CreateCtx cCtx = { 0 };
    if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE);

    if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(key);
        if (cCtx.labelsCount > 0) {
            FreeLabels(cCtx.labels, cCtx.labelsCount);
        }
        return RTS_ReplyGeneralError(ctx, "TSDB: key already exists");
    }

    CreateTsKey(ctx, keyName, &cCtx, &series, &key);
    RedisModule_CloseKey(key);

    RedisModule_Log(ctx, "verbose", "created new series");
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);

    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.create", keyName);

    return REDISMODULE_OK;
}

int TSDB_alter(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    RedisModuleString *keyName = argv[1];
    CreateCtx cCtx = { 0 };
    if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    const GetSeriesResult status = GetSeries(
        ctx, argv[1], &key, &series, REDISMODULE_READ | REDISMODULE_WRITE, GetSeriesFlags_None);
    if (status != GetSeriesResult_Success) {
        return REDISMODULE_ERR;
    }
    if (RMUtil_ArgIndex("RETENTION", argv, argc) > 0) {
        series->retentionTime = cCtx.retentionTime;
    }

    if (RMUtil_ArgIndex("CHUNK_SIZE", argv, argc) > 0) {
        series->chunkSizeBytes = cCtx.chunkSizeBytes;
    }

    if (RMUtil_ArgIndex("DUPLICATE_POLICY", argv, argc) > 0) {
        series->duplicatePolicy = cCtx.duplicatePolicy;
    }

    if (RMUtil_ArgIndex("LABELS", argv, argc) > 0) {
        RemoveIndexedMetric(keyName);
        // free current labels
        FreeLabels(series->labels, series->labelsCount);

        // set new newLabels
        series->labels = cCtx.labels;
        series->labelsCount = cCtx.labelsCount;
        IndexMetric(keyName, series->labels, series->labelsCount);
    }

    if (RMUtil_ArgIndex("IGNORE", argv, argc) > 0) {
        series->ignoreMaxTimeDiff = cCtx.ignoreMaxTimeDiff;
        series->ignoreMaxValDiff = cCtx.ignoreMaxValDiff;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(key);

    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.alter", keyName);

    return REDISMODULE_OK;
}

/*
TS.DELETERULE SOURCE_KEY DEST_KEY
 */
int TSDB_deleteRule(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *srcKeyName = argv[1];

    // First try to remove the rule from the source key
    Series *srcSeries;
    RedisModuleKey *srcKey;
    const GetSeriesResult statusS = GetSeries(ctx,
                                              srcKeyName,
                                              &srcKey,
                                              &srcSeries,
                                              REDISMODULE_READ | REDISMODULE_WRITE,
                                              GetSeriesFlags_DeleteReferences);
    if (statusS != GetSeriesResult_Success) {
        return REDISMODULE_ERR;
    }

    RedisModuleString *destKeyName = argv[2];
    if (!SeriesDeleteRule(srcSeries, destKeyName)) {
        RedisModule_CloseKey(srcKey);
        return RTS_ReplyGeneralError(ctx, "TSDB: compaction rule does not exist");
    }

    // If succeed to remove the rule from the source key remove from the destination too
    Series *destSeries;
    RedisModuleKey *destKey;
    const GetSeriesResult statusD = GetSeries(ctx,
                                              destKeyName,
                                              &destKey,
                                              &destSeries,
                                              REDISMODULE_READ | REDISMODULE_WRITE,
                                              GetSeriesFlags_DeleteReferences);
    if (statusD != GetSeriesResult_Success) {
        RedisModule_CloseKey(srcKey);
        return REDISMODULE_ERR;
    }
    SeriesDeleteSrcRule(destSeries, srcKeyName);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(srcKey);
    RedisModule_CloseKey(destKey);

    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.deleterule:src", srcKeyName);
    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.deleterule:dest", destKeyName);

    return REDISMODULE_OK;
}

/*
TS.CREATERULE sourceKey destKey AGGREGATION aggregationType bucketDuration
*/
int TSDB_createRule(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 6 && argc != 7) {
        return RedisModule_WrongArity(ctx);
    }

    // Validate aggregation arguments
    api_timestamp_t bucketDuration;
    int aggType;
    timestamp_t alignmentTS;
    const int result =
        _parseAggregationArgs(ctx, argv, argc, &bucketDuration, &aggType, NULL, NULL, &alignmentTS);
    if (result == TSDB_NOTEXISTS) {
        return RedisModule_WrongArity(ctx);
    }
    if (result == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    RedisModuleString *srcKeyName = argv[1];
    RedisModuleString *destKeyName = argv[2];
    if (!RedisModule_StringCompare(srcKeyName, destKeyName)) {
        return RTS_ReplyGeneralError(
            ctx, "TSDB: the source key and destination key should be different");
    }

    Series *srcSeries;
    RedisModuleKey *srcKey;
    const GetSeriesFlags flags = GetSeriesFlags_DeleteReferences | GetSeriesFlags_CheckForAcls;
    const GetSeriesResult statusS = GetSeries(
        ctx, srcKeyName, &srcKey, &srcSeries, REDISMODULE_READ | REDISMODULE_WRITE, flags);
    if (statusS != GetSeriesResult_Success) {
        return REDISMODULE_ERR;
    }

    // 1. Verify the source is not a destination
    if (srcSeries->srcKey) {
        RedisModule_CloseKey(srcKey);
        return RTS_ReplyGeneralError(ctx, "TSDB: the source key already has a source rule");
    }

    Series *destSeries;
    RedisModuleKey *destKey;
    const GetSeriesResult statusD = GetSeries(
        ctx, destKeyName, &destKey, &destSeries, REDISMODULE_READ | REDISMODULE_WRITE, flags);
    if (statusD != GetSeriesResult_Success) {
        RedisModule_CloseKey(srcKey);
        return REDISMODULE_ERR;
    }

    // 2. verify dst is not s source
    if (destSeries->rules) {
        RedisModule_CloseKey(srcKey);
        RedisModule_CloseKey(destKey);
        return RTS_ReplyGeneralError(ctx, "TSDB: the destination key already has a dst rule");
    }

    // 3. verify dst doesn't already have src,
    // 4. This covers also the scenario when the rule is already exists
    if (destSeries->srcKey) {
        RedisModule_CloseKey(srcKey);
        RedisModule_CloseKey(destKey);
        return RTS_ReplyGeneralError(ctx, "TSDB: the destination key already has a src rule");
    }

    // add src to dest
    SeriesSetSrcRule(ctx, destSeries, srcSeries->keyName);

    // Last add the rule to source
    if (SeriesAddRule(ctx, srcSeries, destSeries, aggType, bucketDuration, alignmentTS) == NULL) {
        RedisModule_CloseKey(srcKey);
        RedisModule_CloseKey(destKey);
        RedisModule_ReplyWithSimpleString(ctx, "TSDB: ERROR creating rule");
        return REDISMODULE_ERR;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);

    RedisModule_CloseKey(srcKey);
    RedisModule_CloseKey(destKey);

    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.createrule:src", srcKeyName);
    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.createrule:dest", destKeyName);

    return REDISMODULE_OK;
}

/*
TS.INCRBY ts_key NUMBER [TIMESTAMP timestamp]
*/
int TSDB_incrby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *keyName = argv[1];
    Series *series;

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        // the key doesn't exist, lets check we have enough information to create one
        CreateCtx cCtx = { 0 };
        if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }

        CreateTsKey(ctx, keyName, &cCtx, &series, &key);
        SeriesCreateRulesFromGlobalConfig(ctx, keyName, series, cCtx.labels, cCtx.labelsCount);
    } else if (RedisModule_ModuleTypeGetType(key) != SeriesType) {
        return RTS_ReplyGeneralError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    series = RedisModule_ModuleTypeGetValue(key);

    double incrby = 0;
    if (RMUtil_ParseArgs(argv, argc, 2, "d", &incrby) != REDISMODULE_OK) {
        return RTS_ReplyGeneralError(ctx, "TSDB: invalid increase/decrease value");
    }

    long long currentUpdatedTime = -1;
    int timestampLoc = RMUtil_ArgIndex("TIMESTAMP", argv, argc);
    if (timestampLoc == -1 || RMUtil_StringEqualsC(argv[timestampLoc + 1], "*")) {
        currentUpdatedTime = RedisModule_Milliseconds();
    } else if (RedisModule_StringToLongLong(argv[timestampLoc + 1],
                                            (long long *)&currentUpdatedTime) != REDISMODULE_OK) {
        return RTS_ReplyGeneralError(ctx, "TSDB: invalid timestamp");
    }

    if (currentUpdatedTime < series->lastTimestamp && series->lastTimestamp != 0) {
        return RedisModule_ReplyWithError(
            ctx, "TSDB: timestamp must be equal to or higher than the maximum existing timestamp");
    }

    double result = series->lastValue;
    RMUtil_StringToLower(argv[0]);
    bool isIncr = RMUtil_StringEqualsC(argv[0], "ts.incrby");
    if (isIncr) {
        result += incrby;
    } else {
        result -= incrby;
    }

    int rv = internalAdd(ctx, series, currentUpdatedTime, result, DP_LAST, true);
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(key);

    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_GENERIC, isIncr ? "ts.incrby" : "ts.decrby", argv[1]);

    return rv;
}

int TSDB_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2 || argc > 3) {
        return RedisModule_WrongArity(ctx);
    }

    bool latest = false;
    Series *series;
    RedisModuleKey *key;
    const GetSeriesResult status =
        GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ, GetSeriesFlags_None);
    if (status != GetSeriesResult_Success) {
        return REDISMODULE_ERR;
    }

    if (argc == 3) {
        if (parseLatestArg(ctx, argv, argc, &latest) != REDISMODULE_OK || !latest) {
            RTS_ReplyGeneralError(ctx, "TSDB: wrong 3rd argument");
            return REDISMODULE_ERR;
        }
    }

    // LATEST is ignored for a series that is not a compaction.
    bool should_finalize_last_bucket = should_finalize_last_bucket_get(latest, series);
    if (should_finalize_last_bucket) {
        Sample sample;
        Sample *sample_ptr = &sample;
        calculate_latest_sample(&sample_ptr, series);
        if (sample_ptr) {
            ReplyWithSample(ctx, sample.timestamp, sample.value);
        } else {
            ReplyWithSeriesLastDatapoint(ctx, series);
        }
    } else {
        ReplyWithSeriesLastDatapoint(ctx, series);
    }

    RedisModule_CloseKey(key);

    return REDISMODULE_OK;
}

int TSDB_mget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (IsMRCluster()) {
        if (!IsCurrentUserAllowedToReadAllTheKeys(ctx)) {
            return RTS_ReplyKeyPermissionsError(ctx);
        }

        int ctxFlags = RedisModule_GetContextFlags(ctx);

        if (ctxFlags & (REDISMODULE_CTX_FLAGS_LUA | REDISMODULE_CTX_FLAGS_MULTI |
                        REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
            RedisModule_ReplyWithError(ctx,
                                       "Can not run multi sharded command inside a multi exec, "
                                       "lua, or when blocking is not allowed");
            return REDISMODULE_OK;
        }
        return TSDB_mget_RG(ctx, argv, argc);
    }

    RedisModule_AutoMemory(ctx);

    MGetArgs args;
    if (parseMGetCommand(ctx, argv, argc, &args) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    const char **limitLabelsStr = calloc(args.numLimitLabels, sizeof(char *));
    for (int i = 0; i < args.numLimitLabels; i++) {
        limitLabelsStr[i] = RedisModule_StringPtrLen(args.limitLabels[i], NULL);
    }

    bool hasPermissionError = false;
    RedisModuleDict *result = QueryIndex(
        ctx, args.queryPredicates->list, args.queryPredicates->count, &hasPermissionError);

    if (hasPermissionError) {
        free(limitLabelsStr);
        MGetArgs_Free(&args);
        RedisModule_FreeDict(ctx, result);
        RTS_ReplyKeyPermissionsError(ctx);
        return REDISMODULE_ERR;
    }

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    Series *series;
    int exitStatus = REDISMODULE_OK;
    const GetSeriesFlags checkFlags = GetSeriesFlags_SilentOperation | GetSeriesFlags_CheckForAcls;

    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const GetSeriesResult status =
            GetSeries(ctx,
                      RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                      &key,
                      &series,
                      REDISMODULE_READ,
                      checkFlags);

        switch (status) {
            case GetSeriesResult_Success:
                RedisModule_CloseKey(key);
                break;
            case GetSeriesResult_GenericError:
                RedisModule_Log(ctx,
                                "warning",
                                "couldn't open key or key is not a Timeseries. key=%.*s",
                                (int)currentKeyLen,
                                currentKey);
                break;
            case GetSeriesResult_PermissionError:
                RedisModule_Log(
                    ctx,
                    "warning",
                    "The user lacks the required permissions for the key=%.*s, stopping.",
                    (int)currentKeyLen,
                    currentKey);

                RTS_ReplyKeyPermissionsError(ctx);

                exitStatus = REDISMODULE_ERR;
                goto exit;
        }
    }

    RedisModule_ReplyWithMapOrArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN, false);
    RedisModule_DictIteratorStop(iter);
    iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;

        const GetSeriesResult status =
            GetSeries(ctx,
                      RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                      &key,
                      &series,
                      REDISMODULE_READ,
                      GetSeriesFlags_SilentOperation);

        if (status != GetSeriesResult_Success) {
            continue;
        }

        if (!_ReplyMap(ctx)) {
            RedisModule_ReplyWithArray(ctx, 3);
        }
        RedisModule_ReplyWithStringBuffer(ctx, currentKey, currentKeyLen);
        if (_ReplyMap(ctx)) {
            RedisModule_ReplyWithArray(ctx, 2);
        }
        if (args.withLabels) {
            ReplyWithSeriesLabels(ctx, series);
        } else if (args.numLimitLabels > 0) {
            ReplyWithSeriesLabelsWithLimitC(ctx, series, limitLabelsStr, args.numLimitLabels);
        } else {
            RedisModule_ReplyWithMapOrArray(ctx, 0, false);
        }
        // LATEST is ignored for a series that is not a compaction.
        bool should_finalize_last_bucket = should_finalize_last_bucket_get(args.latest, series);
        if (should_finalize_last_bucket) {
            Sample sample;
            Sample *sample_ptr = &sample;
            calculate_latest_sample(&sample_ptr, series);
            if (sample_ptr) {
                ReplyWithSample(ctx, sample.timestamp, sample.value);
            } else {
                ReplyWithSeriesLastDatapoint(ctx, series);
            }
        } else {
            ReplyWithSeriesLastDatapoint(ctx, series);
        }
        replylen++;
        RedisModule_CloseKey(key);
    }

exit:
    if (exitStatus == REDISMODULE_OK) {
        RedisModule_ReplySetMapOrArrayLength(ctx, replylen, false);
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_FreeDict(ctx, result);
    MGetArgs_Free(&args);
    free(limitLabelsStr);
    return exitStatus;
}

static inline bool is_obsolete(timestamp_t ts,
                               timestamp_t lastTimestamp,
                               timestamp_t retentionTime) {
    return (lastTimestamp > retentionTime) && (ts < lastTimestamp - retentionTime);
}

static inline bool verify_compaction_del_possible(RedisModuleCtx *ctx,
                                                  const Series *series,
                                                  const RangeArgs *args) {
    bool is_valid = true;
    if (!series->rules || !series->retentionTime)
        return true;

    // Verify startTimestamp in retention period
    if (is_obsolete(args->startTimestamp, series->lastTimestamp, series->retentionTime)) {
        is_valid = false;
    }

    // Verify all compaction's buckets are in the retention period
    CompactionRule *rule = series->rules;
    while (rule != NULL) {
        const timestamp_t ruleTimebucket = rule->bucketDuration;
        const timestamp_t curAggWindowStart = BucketStartNormalize(
            CalcBucketStart(args->startTimestamp, ruleTimebucket, rule->timestampAlignment));
        if (is_obsolete(curAggWindowStart, series->lastTimestamp, series->retentionTime)) {
            is_valid = false;
        }
        rule = rule->nextRule;
    }

    if (unlikely(!is_valid)) {
        RTS_ReplyGeneralError(ctx,
                              "TSDB: When a series has compactions, deleting samples or compaction "
                              "buckets beyond the series retention period is not possible");
    }

    return is_valid;
}

int TSDB_delete(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    RangeArgs args = { 0 };
    if (parseRangeArguments(ctx, 2, argv, argc, &args) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    Series *series;
    RedisModuleKey *key;
    const GetSeriesResult status = GetSeries(
        ctx, argv[1], &key, &series, REDISMODULE_READ | REDISMODULE_WRITE, GetSeriesFlags_None);
    if (status != GetSeriesResult_Success) {
        return REDISMODULE_ERR;
    }

    if (unlikely(!verify_compaction_del_possible(ctx, series, &args))) {
        RedisModule_CloseKey(key);
        return REDISMODULE_ERR;
    }

    size_t deleted = SeriesDelRange(series, args.startTimestamp, args.endTimestamp);

    RedisModule_ReplyWithLongLong(ctx, deleted);
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.del", argv[1]);

    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

void FlushEventCallback(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    if ((!memcmp(&eid, &RedisModuleEvent_FlushDB, sizeof(eid))) &&
        subevent == REDISMODULE_SUBEVENT_FLUSHDB_END) {
        RemoveAllIndexedMetrics();
    }
}

void swapDbEventCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data) {
    RedisModule_Log(ctx, "warning", "swapdb isn't supported by redis timeseries");
    if ((!memcmp(&e, &RedisModuleEvent_FlushDB, sizeof(e)))) {
        RedisModuleSwapDbInfo *ei = data;
        REDISMODULE_NOT_USED(ei);
    }
}

void keyAddedToDbDict(RedisModuleCtx *ctx,
                      RedisModuleString *key,
                      void *value,
                      int swap_key_metadata) {
    Series *series = (Series *)value;
    series->in_ram = true;
}

int keyRemovedFromDbDict(RedisModuleCtx *ctx,
                         RedisModuleString *key,
                         void *value,
                         int swap_key_metadata,
                         int writing_to_swap) {
    Series *series = (Series *)value;
    if (!!writing_to_swap) {
        series->in_ram = false;
    }
    return 0;
}

int persistence_in_progress = 0;

void persistCallback(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    if (memcmp(&eid, &RedisModuleEvent_Persistence, sizeof(eid)) != 0) {
        return;
    }

    if (subevent == REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START ||
        subevent == REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START ||
        subevent == REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START ||
        subevent == REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START) {
        persistence_in_progress++;
    } else if (subevent == REDISMODULE_SUBEVENT_PERSISTENCE_ENDED ||
               subevent == REDISMODULE_SUBEVENT_PERSISTENCE_FAILED) {
        persistence_in_progress--;
    }

    return;
}

void ShardingEvent(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    /**
     * On sharding event we need to do couple of things depends on the subevent given:
     *
     * 1. REDISMODULE_SUBEVENT_SHARDING_SLOT_RANGE_CHANGED
     *    On this event we know that the slot range changed and we might have data
     *    which are no longer belong to this shard, we must ignore it on searches
     *
     * 2. REDISMODULE_SUBEVENT_SHARDING_TRIMMING_STARTED
     *    This event tells us that the trimming process has started and keys will start to be
     *    deleted, we do not need to do anything on this event
     *
     * 3. REDISMODULE_SUBEVENT_SHARDING_TRIMMING_ENDED
     *    This event tells us that the trimming process has finished, we are not longer
     *    have data that are not belong to us and its safe to stop checking this on searches.
     */
    if (eid.id != REDISMODULE_EVENT_SHARDING) {
        RedisModule_Log(rts_staticCtx, "warning", "Bad event given, ignored.");
        return;
    }

    switch (subevent) {
        case REDISMODULE_SUBEVENT_SHARDING_SLOT_RANGE_CHANGED:
            RedisModule_Log(
                ctx, "notice", "%s", "Got slot range change event, enter trimming phase.");
            isTrimming = true;
            break;
        case REDISMODULE_SUBEVENT_SHARDING_TRIMMING_STARTED:
            RedisModule_Log(
                ctx, "notice", "%s", "Got trimming started event, enter trimming phase.");
            isTrimming = true;
            break;
        case REDISMODULE_SUBEVENT_SHARDING_TRIMMING_ENDED:
            RedisModule_Log(ctx, "notice", "%s", "Got trimming ended event, exit trimming phase.");
            isTrimming = false;
            break;
        default:
            RedisModule_Log(rts_staticCtx, "warning", "Bad subevent given, ignored.");
    }
}

void ReplicaBackupCallback(RedisModuleCtx *ctx,
                           RedisModuleEvent eid,
                           uint64_t subevent,
                           void *data) {
    REDISMODULE_NOT_USED(eid);
    switch (subevent) {
        case REDISMODULE_SUBEVENT_REPL_BACKUP_CREATE:
            Backup_Globals();
            break;
        case REDISMODULE_SUBEVENT_REPL_BACKUP_RESTORE:
            Restore_Globals();
            break;
        case REDISMODULE_SUBEVENT_REPL_BACKUP_DISCARD:
            Discard_Globals_Backup();
            break;
    }
}

bool CheckVersionForBlockedClientMeasureTime() {
    // Minimal versions: 6.2.0
    if (RTS_currVersion.redisMajorVersion > 6)
        return true;
    if (RTS_currVersion.redisMajorVersion == 6 && RTS_currVersion.redisMinorVersion >= 2)
        return true;
    return false;
}

int CheckVersionForShortRead() {
    // Minimal versions: 6.2.5
    // (6.0.15 is not supporting the required event notification for modules)
    if (RTS_currVersion.redisMajorVersion > 6)
        return REDISMODULE_OK;
    if (RTS_currVersion.redisMajorVersion == 6 && RTS_currVersion.redisMinorVersion > 2)
        return REDISMODULE_OK;
    if (RTS_currVersion.redisMajorVersion == 6 && RTS_currVersion.redisMinorVersion == 2 &&
        RTS_currVersion.redisPatchVersion >= 5)
        return REDISMODULE_OK;
    return REDISMODULE_ERR;
}

void Initialize_RdbNotifications(RedisModuleCtx *ctx) {
    if (CheckVersionForShortRead() == REDISMODULE_OK) {
        int success = RedisModule_SubscribeToServerEvent(
            ctx, RedisModuleEvent_ReplBackup, ReplicaBackupCallback);
        RedisModule_Assert(success !=
                           REDISMODULE_ERR); // should be supported in this redis version/release
        RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);
        RedisModule_Log(ctx, "notice", "Enabled diskless replication");
    }
}

__attribute__((weak)) int (*RedisModule_SetDataTypeExtensions)(
    RedisModuleCtx *ctx,
    RedisModuleType *mt,
    RedisModuleTypeExtMethods *typemethods) REDISMODULE_ATTR = NULL;

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    if (rts_staticCtx) {
        FreeConfig();

        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;
    }

    return REDISMODULE_OK;
}

// Some of the defrag funcionality might be missing in different versions of redis (e.g., redis 8 + RoF).
// To avoid calling unimplemented functions we prepare do-nothing stubs for the defrag registration functions
// and pointthe missing function pointers to them.
static int Stub_RegisterDefragFunc(RedisModuleCtx *ctx, RedisModuleDefragFunc func) {
    return REDISMODULE_OK;
}
static int Stub_RegisterDefragFunc2(RedisModuleCtx *ctx, RedisModuleDefragFunc2 func) {
    return REDISMODULE_OK;
}
static int Stub_RegisterDefragCallbacks(RedisModuleCtx *ctx, RedisModuleDefragFunc start, RedisModuleDefragFunc end) {
    return REDISMODULE_OK;
}
/*
module loading function, possible arguments:
COMPACTION_POLICY - compaction policy from parse_policies,h
RETENTION_POLICY - long that represents the retention in milliseconds
MAX_SAMPLE_PER_CHUNK - how many samples per chunk
example:
redis-server --loadmodule ./redistimeseries.so COMPACTION_POLICY
"max:1m:1d;min:10s:1h;avg:2h:10d;avg:3d:100d" RETENTION_POLICY 3600 MAX_SAMPLE_PER_CHUNK 1024
*/
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "timeseries", REDISTIMESERIES_MODULE_VERSION, REDISMODULE_APIVER_1) ==
        REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_RegisterDefragFunc == NULL)
        RedisModule_RegisterDefragFunc = Stub_RegisterDefragFunc;
    if (RedisModule_RegisterDefragFunc2 == NULL)
        RedisModule_RegisterDefragFunc2 = Stub_RegisterDefragFunc2;
    if (RedisModule_RegisterDefragCallbacks == NULL)
        RedisModule_RegisterDefragCallbacks = Stub_RegisterDefragCallbacks;

    rts_staticCtx = RedisModule_GetDetachedThreadSafeContext(ctx);

    RedisModule_Log(ctx,
                    "notice",
                    "RedisTimeSeries version %d, git_sha=%s",
                    REDISTIMESERIES_MODULE_VERSION,
                    REDISTIMESERIES_GIT_SHA);

    RTS_GetRedisVersion();
    RedisModule_Log(ctx,
                    "notice",
                    "Redis version found by RedisTimeSeries : %d.%d.%d - %s",
                    RTS_currVersion.redisMajorVersion,
                    RTS_currVersion.redisMinorVersion,
                    RTS_currVersion.redisPatchVersion,
                    RTS_IsEnterprise() ? "enterprise" : "oss");
    if (RTS_IsEnterprise()) {
        RedisModule_Log(ctx,
                        "notice",
                        "Redis Enterprise version found by RedisTimeSeries : %d.%d.%d-%d",
                        RTS_RlecMajorVersion,
                        RTS_RlecMinorVersion,
                        RTS_RlecPatchVersion,
                        RTS_RlecBuild);
    }

    if (RTS_CheckSupportedVestion() != REDISMODULE_OK) {
        RedisModule_Log(ctx,
                        "warning",
                        "Redis version is too old, please upgrade to redis "
                        "%d.%d.%d and above.",
                        RTS_minSupportedVersion.redisMajorVersion,
                        RTS_minSupportedVersion.redisMinorVersion,
                        RTS_minSupportedVersion.redisPatchVersion);
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    if (!LoadConfiguration(ctx, argv, argc)) {
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    initGlobalCompactionFunctions();

    if (register_rg(ctx, TSGlobalConfig.numThreads) != REDISMODULE_OK) {
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = series_rdb_load,
        .rdb_save = series_rdb_save,
        .aof_rewrite = RMUtil_DefaultAofRewrite,
        .mem_usage = SeriesMemUsage,
        .copy = CopySeries,
        .free = FreeSeries,
        .defrag = DefragSeries,
    };

    SeriesType = RedisModule_CreateDataType(ctx, "TSDB-TYPE", TS_LATEST_ENCVER, &tm);
    if (SeriesType == NULL) {
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    RedisModuleTypeExtMethods etm = {
        .version = REDISMODULE_TYPE_EXT_METHOD_VERSION,
        .key_added_to_db_dict = keyAddedToDbDict,
        .removing_key_from_db_dict = keyRemovedFromDbDict,
        .get_key_metadata_for_rdb = NULL,
    };
    if (RedisModule_SetDataTypeExtensions != NULL) {
        if (RedisModule_SetDataTypeExtensions(ctx, SeriesType, &etm) != REDISMODULE_OK) {
            FreeConfig();
            RedisModule_FreeThreadSafeContext(rts_staticCtx);
            rts_staticCtx = NULL;

            return REDISMODULE_ERR;
        }
    }

    if (RedisModule_AddACLCategory &&
        RedisModule_AddACLCategory(ctx, TIMESERIES_MODULE_ACL_CATEGORY_NAME) != REDISMODULE_OK) {
        RedisModule_Log(ctx, "warning", "Failed to add ACL category");

        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    IndexInit();
    if (RedisModule_RegisterDefragFunc2(ctx, DefragIndex) != REDISMODULE_OK) {
        RedisModule_Log(ctx, "warning", "Failed to register defrag function");
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    RegisterCommandWithModesAndAcls(ctx, "ts.create", TSDB_create, "write deny-oom", "write fast");
    RegisterCommandWithModesAndAcls(ctx, "ts.alter", TSDB_alter, "write deny-oom", "write");
    RegisterCommandWithModesAndAcls(ctx, "ts.createrule", TSDB_createRule, "write fast", "write");
    RegisterCommandWithModesAndAcls(ctx, "ts.deleterule", TSDB_deleteRule, "write", "write fast");
    RegisterCommandWithModesAndAcls(ctx, "ts.add", TSDB_add, "write deny-oom", "write");
    RegisterCommandWithModesAndAcls(ctx, "ts.incrby", TSDB_incrby, "write deny-oom", "write");
    RegisterCommandWithModesAndAcls(ctx, "ts.decrby", TSDB_incrby, "write deny-oom", "write");
    RegisterCommandWithModesAndAcls(ctx, "ts.range", TSDB_range, "readonly", "read");
    RegisterCommandWithModesAndAcls(ctx, "ts.revrange", TSDB_revrange, "readonly", "read");

    if (RedisModule_CreateCommand(ctx, "ts.queryindex", TSDB_queryindex, "readonly", 0, 0, -1) ==
        REDISMODULE_ERR) {
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    SetCommandAcls(ctx, "ts.queryindex", "read");

    RegisterCommandWithModesAndAcls(ctx, "ts.info", TSDB_info, "readonly", "read fast");
    RegisterCommandWithModesAndAcls(ctx, "ts.get", TSDB_get, "readonly", "read fast");
    RegisterCommandWithModesAndAcls(ctx, "ts.del", TSDB_delete, "write", "write");

    if (RedisModule_CreateCommand(ctx, "ts.madd", TSDB_madd, "write deny-oom", 1, -1, 3) ==
        REDISMODULE_ERR) {
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    SetCommandAcls(ctx, "ts.madd", "write");

    if (RedisModule_CreateCommand(ctx, "ts.mrange", TSDB_mrange, "readonly", 0, 0, -1) ==
        REDISMODULE_ERR) {
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    SetCommandAcls(ctx, "ts.mrange", "read");

    if (RedisModule_CreateCommand(ctx, "ts.mrevrange", TSDB_mrevrange, "readonly", 0, 0, -1) ==
        REDISMODULE_ERR) {
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    SetCommandAcls(ctx, "ts.mrevrange", "read");

    if (RedisModule_CreateCommand(ctx, "ts.mget", TSDB_mget, "readonly", 0, 0, -1) ==
        REDISMODULE_ERR) {
        FreeConfig();
        RedisModule_FreeThreadSafeContext(rts_staticCtx);
        rts_staticCtx = NULL;

        return REDISMODULE_ERR;
    }

    SetCommandAcls(ctx, "ts.mget", "read");

    if (RedisModule_SubscribeToServerEvent && RedisModule_ShardingGetKeySlot) {
        // we have server events support, lets subscribe to relevan events.
        RedisModule_Log(ctx, "notice", "%s", "Subscribe to sharding events");
        RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Sharding, ShardingEvent);
    }

    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_FlushDB, FlushEventCallback);
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_SwapDB, swapDbEventCallback);
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Persistence, persistCallback);

    Initialize_RdbNotifications(ctx);

    return REDISMODULE_OK;
}

#if (defined(DEBUG) || defined(_DEBUG)) && !defined(NDEBUG)
#include "readies/cetara/diag/gdb.c"
#endif
