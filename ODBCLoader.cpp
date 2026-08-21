/* Copyright (c) 2005 - 2012 Vertica, an HP company -*- C++ -*- */
// vim:ru:sm:ts=4:et:tw=0

#include "Vertica.h"
#include "StringParsers.h"
#include <sql.h>
#include <time.h>
#include <sqlext.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <iostream>
#include <vector>
#include <sstream>
#include <regex>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <algorithm>

// To deal with TimeTz and TimestampTz.
// No standard native-SQL representation for these,
// so we ask for them as strings and re-parse them.
// (Ew...)
#include "StringParsers.h"

// To support Vertica SDK before 9.3:
#ifndef SDK_BUILD_ASSERTIONS_H // conveniently doesn't exist before 9.3
#define parseTimeTz(a,b,c,d,e,f) parseTimeTz(a,b,c,d,e)
#define parseTimestampTz(a,b,c,d,e,f) parseTimestampTz(a,b,c,d,e)
#define parseNumeric(a,b,c,d,e,f) parseNumeric(a,b,c,d,e)
#endif

#define MIN_ROWSET  1       // Min rowset value
#define MAX_ROWSET  10000   // Max rowset value
#define DEF_ROWSET  100     // Default rowset
#define MIN_THREAD  1       // Min thread_count value
#define MAX_THREAD  64      // Max thread_count value
#define DEF_THREAD  1       // Default thread_count
#define MAX_QUEUE_BATCHES 8 // Queue depth; bounds buffered rows at 8 x rowset
#define BATCHES_PER_BREAK 4 // Batches per process() call, to keep cancel checks frequent
#define MAX_PRELEN  2048    // Max predicate length
#define MAX_PRENUM  10      // Max predicate number
#define REG_CASTRM  R"(::\w+(\([^()]*\))*)"
#define REG_ANYMTC  R"(\s*=\s*ANY\s*\(ARRAY\[([^\]]*)\])"
#define REG_ANYREP  " IN($1)"
#define REG_TILDEM  R"(\s*~~\s*)"
#define REG_TILDER  " LIKE "
#define REG_ENDSCO  R"(\s*;\s*$)"
#define REG_QUERYP  R"(^\s*\(*\s*override_query\s*<\s*'\s*([\s\S]*)\s*'[\s\S]*$)"

using namespace Vertica;

// ii declare global variable colInTable (# columns in source table, vidx (array conataining index of column in SELECT)
int colInTable = 0;
std::vector<int> vidx;
//
static inline TimeADT getTimeFromHMS(uint32 hour, uint8 min, uint8 sec) {
    return getTimeFromUnixTime(sec + min*60 + hour*3600);
}

class ODBCLoader : public UDParser {
public:
    ODBCLoader() : currentSlice(0), quirks(NoQuirks), modSupported(false),
                   threaded(false), workersStarted(false), threadCountParam(DEF_THREAD) {}

    // Maximum length of diagnostic-message text
    // that we can receive from the ODBC driver.
    // Currently must fit on the stack.
    // Diagnostics messages are expected to be short,
    // but the spec does not define a max length.
    static const uint32 MAX_DIAG_MSG_TEXT_LENGTH = 1024;

    // Periodically, we need to break out of our
    // loop fetching data from the remote server and
    // let Vertica do some accounting.  (Mostly check
    // to see if this query has been cancelled.)
    // This knob sets how many times we should try to
    // read another row before doing so.
    // Each such break incurs the cost of a C++
    // virtual function call; this number should be
    // big enough to effectively amortize that cost.
    static const uint32 ROWS_PER_BREAK = 10000;

private:
    // Keep a copy of the information about each column.
    // Note that Vertica doesn't let us safely keep a reference to
    // the internal copy of this data structure that it shows us.
    // But keeping a copy is fine.
    SizedColumnTypes colInfo;

    // ODBC connection/query state
    SQLHENV env;
    SQLHDBC dbc;
    SQLHSTMT stmt;
    SQLSMALLINT numcols;
    SQLULEN nfrows;		// Number of fetched rows
    size_t rowset;

    // One SQL string per slice; a single entry means no split.
    std::vector<std::string> sliceQueries;
    int currentSlice;

    enum PerDBQuirks {
        NoQuirks = 0,
        Oracle
    };

    PerDBQuirks quirks;

    // Whether the remote engine supports MOD(); set in setQuirksMode().
    bool modSupported;

    // MF keeping this to re-use the code in the Fetch loop...
    struct Buf {
        SQLLEN len;
        SQLPOINTER buf;
    };

    //std::vector<Buf> col_data_bufs;
    // MF we're going to use rowset in "column binding format" so we need
    //    for each retrieved column two arrays:
    //    one containing "rowset" results
    //    one containing "rowset" length indicators
    //    resp and len are the pointers to the pointers array.
    SQLPOINTER *resp ;     // result array pointers pointer
    SQLLEN **lenp ;        // length array pointers pointer

    // MF we want to determine Vertica/ODBC types & sizes once and for all...
    BaseDataOID *vtype ;  // Vertica types pointer
    uint32      *stype ;  // Vertica data type size
    SQLSMALLINT *ctype ;  // ODBC C type; precomputed so workers never call the SDK for it

    StringParsers parser;

    // Worker threads open their own connections with this same string.
    std::string connect;

    // ---- Threaded parallel-fetch state (US 5598915 + US 5602339) ----

    // One column of one row, as raw driver bytes the main thread converts later.
    struct Cell {
        bool isNull;
        SQLLEN lenIndicator;  // original driver length indicator (may be SQL_NTS)
        std::string bytes;
        Cell() : isNull(true), lenIndicator(0) {}
    };
    struct Batch {
        std::vector<std::vector<Cell> > rows;
    };

    // One bounded queue shared by all workers; the main thread is the sole consumer.
    struct BatchQueue {
        std::mutex mtx;
        std::condition_variable notFull;
        std::condition_variable notEmpty;
        std::deque<Batch> items;
        size_t maxItems;
        int activeProducers;    // workers not yet finished
        bool shutdown;          // cancel/teardown: wake blocked workers
        BatchQueue() : maxItems(MAX_QUEUE_BATCHES), activeProducers(0), shutdown(false) {}
    };

    BatchQueue queue;
    std::vector<std::thread> workers;

    // Per-worker error marshalling: captured as DATA, never thrown across threads.
    struct WorkerStatus {
        bool failed;
        std::string message;    // driver message
        std::string sqlstate;   // SQLSTATE
        WorkerStatus() : failed(false) {}
    };
    std::vector<WorkerStatus> workerStatus;

    bool threaded;          // true when running the multi-worker path
    bool workersStarted;    // spawn workers only once across process() re-entry
    int threadCountParam;   // validated thread_count; hard ceiling on workers

    // Gets the Vertica type of the specified column
    VerticaType getVerticaTypeOfCol(SQLSMALLINT colnum) {
        return colInfo.getColumnType(colnum);
    }

    // Gets the ODBC type of the specified column
    SQLSMALLINT getODBCTypeOfCol(SQLSMALLINT colnum) {
        VerticaType type = getVerticaTypeOfCol(colnum);
        switch (type.getTypeOid()) {
        case BoolOID:           return SQL_BIT;
        case Int8OID:           return SQL_BIGINT;
        case Float8OID:         return SQL_DOUBLE;
        case CharOID:           return SQL_CHAR;
        case VarcharOID:        return SQL_LONGVARCHAR;
        case DateOID:           return SQL_DATE;
        case TimeOID:           return SQL_TIME;
        case TimestampOID:      return SQL_TIMESTAMP;
        case TimestampTzOID:    return SQL_VARCHAR;  // Don't know how to deal with timezones in ODBC; just get them as a string and parse it
        case IntervalOID:       return SQL_INTERVAL_DAY_TO_SECOND;
        case IntervalYMOID:     return SQL_INTERVAL_YEAR_TO_MONTH;
        case TimeTzOID:         return SQL_VARCHAR;  // Don't know how to deal with timezones in ODBC; just get them as a string and parse it
        case NumericOID:        return SQL_NUMERIC;
        case BinaryOID:         return SQL_BINARY;
        case VarbinaryOID:      return SQL_LONGVARBINARY;

#ifndef NO_LONG_OIDS
	case LongVarbinaryOID:  return SQL_LONGVARBINARY;
	case LongVarcharOID:    return SQL_LONGVARCHAR;
#endif // NO_LONG_OIDS

        default:                vt_report_error(0, "Unrecognized Vertica type: %s (OID %llu)", type.getTypeStr(), type.getTypeOid()); return SQL_UNKNOWN_TYPE;  // Should never get here; vt_report_error() shouldn't return
        }
    }

    // Gets the ODBC C data-type identifier for the specified column
    SQLSMALLINT getCTypeOfCol(SQLSMALLINT colnum) {
        VerticaType type = getVerticaTypeOfCol(colnum);
        switch (type.getTypeOid()) {
        case BoolOID:           return SQL_C_BIT;
        case Int8OID:           return (quirks != Oracle ? SQL_C_SBIGINT : SQL_C_CHAR);
        case Float8OID:         return SQL_C_DOUBLE;
        case CharOID:           return SQL_C_CHAR;
        case VarcharOID:        return SQL_C_CHAR;
        case DateOID:           return SQL_C_DATE;
        case TimeOID:           return SQL_C_TIME;
        case TimestampOID:      return SQL_C_TIMESTAMP;
        case TimestampTzOID:    return SQL_C_CHAR;  // Don't know how to deal with timezones in ODBC; just get them as a string and parse it
        case IntervalOID:       return SQL_C_INTERVAL_DAY_TO_SECOND;
        case IntervalYMOID:     return SQL_C_INTERVAL_YEAR_TO_MONTH;
        case TimeTzOID:         return SQL_C_CHAR;  // Don't know how to deal with timezones in ODBC; just get them as a string and parse it
        case NumericOID:        return SQL_C_CHAR;
        case BinaryOID:         return SQL_C_BINARY;
        case VarbinaryOID:      return SQL_C_BINARY;

#ifndef NO_LONG_OIDS
	case LongVarbinaryOID:  return SQL_C_BINARY;
	case LongVarcharOID:    return SQL_C_CHAR;
#endif // NO_LONG_OIDS

        default:                vt_report_error(0, "Unrecognized Vertica type %s (OID: %llu)", type.getTypeStr(), type.getTypeOid()); return SQL_UNKNOWN_TYPE;  // Should never get here; vt_report_error() shouldn't return
        }
    }

    // Return the size of the memory allocation needed to store ODBC data for column 'colnum'
    uint32 getFieldSizeForCol(SQLSMALLINT colnum) {
        VerticaType type = getVerticaTypeOfCol(colnum);
        switch (type.getTypeOid()) {
        // Everything fixed-length is the same size in Vertica as ODBC
        case BoolOID: case Int8OID: case Float8OID:
            return type.getMaxSize();

        // Everything string-based is the same size too.
        // Except ODBC may decide that we want a trailing null terminator.
        case CharOID: case VarcharOID: case BinaryOID: case VarbinaryOID:
	
#ifndef NO_LONG_OIDS
	case LongVarbinaryOID: case LongVarcharOID:
#endif // NO_LONG_OIDS

            return type.getMaxSize() + 1;

        // Numeric is a special beast
        // Needs to be size of their header plus our(/their) data
        // Let's be lazy for now and just do their header plus our total size (includes our header)
        // EDIT: Just use strings for Numeric's as well; some DB's seem to have trouble scaling them.
        case NumericOID:
            return 128;

        // Things represented as char's because there's no good native type
        // could be just about any length.
        // So just make something up; hope it's long enough.
        case TimestampTzOID: case TimeTzOID:
            return 80;
            
        // Everything struct-based needs to be the size of that struct
        case DateOID: return sizeof(DATE_STRUCT);
        case TimeOID: return sizeof(TIME_STRUCT);
        case TimestampOID: return sizeof(TIMESTAMP_STRUCT);
        case IntervalOID: case IntervalYMOID: return sizeof(SQL_INTERVAL_STRUCT);
        
        // Otherwise it's a type we don't know about
        default: vt_report_error(0, "Unrecognized Vertica type: %s (OID: %llu)", type.getTypeStr(), type.getTypeOid()); return (uint32)-1;  // Should never get here; vt_report_error() shouldn't return
        }
    }

    void handleReturnCode(ServerInterface &srvInterface, int r, SQLSMALLINT handle_type, SQLHANDLE handle, const char *fn_name) {
        // Check for error codes; retrieve error messages if any
        bool error = false;
        switch (r) {
        case SQL_SUCCESS: return;
            
        case SQL_ERROR: error = true;  // Fall through
        case SQL_SUCCESS_WITH_INFO: {
            SQLCHAR state_rec[6];
            SQLINTEGER native_code;
            SQLCHAR message_text[MAX_DIAG_MSG_TEXT_LENGTH];
            SQLSMALLINT msg_length;
            SQLRETURN r_diag = SQLGetDiagRec(handle_type, handle, 1, &state_rec[0], &native_code,
                                             &message_text[0], MAX_DIAG_MSG_TEXT_LENGTH, &msg_length);

            // No infinite loops!
            // Throw out secondary 'info' messages;
            // if our process for fetching info messages generates info messages,
            // we'll be at it for a while...
            if (r_diag != SQL_SUCCESS && r_diag != SQL_SUCCESS_WITH_INFO) {
                if (error) {
                    vt_report_error(0, "ODBC Error:  Error reported attempting to get the error message for another error!  Unable to display the error message.  Original error was in function %s.", fn_name);
                } else {
                    srvInterface.log("ODBC Warning:  Error reported attempting to get the warning message for another operation!  Unable to display the warning message.  Original warning was in function %s.", fn_name);
                }
            }
            
            const char *truncated = (msg_length > (SQLSMALLINT)MAX_DIAG_MSG_TEXT_LENGTH ? "... (message truncated)" : "");
            
            if (error) {
                vt_report_error(0, "ODBC Error: %s failed with error code %s, native code %d [%s%s]",
                                fn_name, &state_rec[0], (int)native_code, &message_text[0], truncated);
            } else {
                srvInterface.log("ODBC Warning: %s emitted a warning with error code %s, native code %d [%s%s]",
                                 fn_name, &state_rec[0], (int)native_code, &message_text[0], truncated);
            }

            break;
        }
            
        case SQL_INVALID_HANDLE: vt_report_error(0, "ODBC Error: %s failed with internal error SQL_INVALID_HANDLE", fn_name); break;
            
        case SQL_STILL_EXECUTING: vt_report_error(0, "ODBC Error: Synchronous function %s returned SQL_STILL_EXECUTING", fn_name); break;

        case SQL_NO_DATA: vt_report_error(0, "ODBC Error: %s returned SQL_NO_DATA.  Were we cancelled remotely?", fn_name); break;

        case SQL_NEED_DATA: vt_report_error(0, "ODBC Error: %s eturned SQL_NEED_DATA.  Are we calling a stored procedure?  We do not provide parameter values to remote databases; arguments must be hardcoded.", fn_name); break;

// TODO: Apparently this isn't defined but is a valid return code sometimes?
//        case SQL_PARAM_DATA_AVAILABLE: vt_report_error(0, "ODBC Error: Returned SQL_PARAM_DATA_AVAILABLE.  Remote server wants us to handle ODBC Parameters that we didn't set.");

        default: vt_report_error(0,
                                 "ODBC Error: Invalid return code from %s: %d.  " \
                                 "Expected values are %d (SQL_SUCCESS), %d (SQL_SUCCESS_WITH_INFO), %d (SQL_ERROR), " \
                                 "%d (SQL_INVALID_HANDLE), %d (SQL_STILL_EXECUTING), %d (SQL_NO_DATA), or %d (SQL_NEED_DATA).",
                                 fn_name, r, SQL_SUCCESS, SQL_SUCCESS_WITH_INFO, SQL_ERROR,
                                 SQL_INVALID_HANDLE, SQL_STILL_EXECUTING, SQL_NO_DATA, SQL_NEED_DATA);
        }
    }

    // Strict base-10 parse; rejects non-integer input (text, dates, decimals).
    static bool parseWholeInteger(const char *s, long long &out) {
        if (s == NULL) return false;
        while (*s == ' ') s++;
        if (*s == '\0') return false;
        char *end = NULL;
        long long v = strtoll(s, &end, 10);
        if (end == s) return false;             // no digits consumed
        while (*end == ' ') end++;              // tolerate trailing spaces
        if (*end != '\0') return false;         // trailing junk -> not an integer
        out = v;
        return true;
    }

    // Probes MIN/MAX split bounds; returns false on any error so the caller can fall back.
    bool probeSplitBounds(ServerInterface &srvInterface, const std::string &baseQuery,
                          const std::string &splitColumn, long long &lo, long long &hi) {
        std::string probe = "SELECT MIN(" + splitColumn + "), MAX(" + splitColumn +
                            ") FROM ( " + baseQuery + " ) t";
        SQLHSTMT pstmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &pstmt))) {
            return false;
        }

        bool ok = false;
        if (SQL_SUCCEEDED(SQLExecDirect(pstmt, (SQLCHAR*)probe.c_str(), SQL_NTS))) {
            char minbuf[128] = {0};
            char maxbuf[128] = {0};
            SQLLEN minlen = 0, maxlen = 0;
            // Fetch as strings so we can validate integer-ness ourselves.
            SQLBindCol(pstmt, 1, SQL_C_CHAR, minbuf, sizeof(minbuf), &minlen);
            SQLBindCol(pstmt, 2, SQL_C_CHAR, maxbuf, sizeof(maxbuf), &maxlen);
            if (SQL_SUCCEEDED(SQLFetch(pstmt)) &&
                minlen != SQL_NULL_DATA && maxlen != SQL_NULL_DATA &&
                parseWholeInteger(minbuf, lo) && parseWholeInteger(maxbuf, hi)) {
                ok = (lo <= hi);
            }
        }

        SQLFreeStmt(pstmt, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, pstmt);
        return ok;
    }

    // Builds per-slice queries; falls back to a single slice when a split is unavailable.
    void buildSliceQueries(ServerInterface &srvInterface, const std::string &baseQuery,
                           int threadCount, const std::string &splitColumn,
                           const std::string &splitMethod) {
        sliceQueries.clear();
        currentSlice = 0;

        // No split requested -> original single-connection path, unchanged.
        if (splitColumn.empty() || threadCount <= 1) {
            if (threadCount > 1 && splitColumn.empty()) {
                srvInterface.log("ODBC Loader: thread_count=%d ignored because split_column is not set; single-connection load", threadCount);
            }
            sliceQueries.push_back(baseQuery);
            return;
        }

        // Bare identifiers only; anything else falls back to single-connection.
        static const std::regex re_ident("^[A-Za-z_][A-Za-z0-9_]*$", std::regex::ECMAScript);
        long long lo = 0, hi = 0;
        if (!std::regex_match(splitColumn, re_ident) ||
            !probeSplitBounds(srvInterface, baseQuery, splitColumn, lo, hi)) {
            srvInterface.log("ODBC Loader: split_column '%s' unusable (missing, non-integer, reserved/quoted, or pruned); falling back to single-connection load",
                             splitColumn.c_str());
            sliceQueries.push_back(baseQuery);
            return;
        }

        // MOD() is engine-dependent; range is the portable default.
        bool useModulo = (splitMethod == "modulo");
        if (useModulo && !modSupported) {
            srvInterface.log("ODBC Loader: modulo split not supported on this engine; downgraded to range split");
            useModulo = false;
        }

        if (useModulo) {
            // Double-MOD normalizes negative keys into 0..N-1.
            for (int k = 0; k < threadCount; k++) {
                std::ostringstream q;
                q << "SELECT * FROM ( " << baseQuery << " ) t WHERE (MOD(MOD("
                  << splitColumn << ", " << threadCount << ") + " << threadCount
                  << ", " << threadCount << ") = " << k << ")";
                sliceQueries.push_back(q.str());
            }
        } else {
            // Range split: contiguous [lo, hi] chunks; __int128 avoids overflow.
            __int128 count = (__int128)hi - (__int128)lo + 1;
            for (int k = 0; k < threadCount; k++) {
                long long startIdx = (long long)((count * k) / threadCount);
                long long endIdx   = (long long)((count * (k + 1)) / threadCount);
                if (startIdx >= endIdx) continue;   // more slices than distinct values
                long long sliceLo = lo + startIdx;
                long long sliceHi = lo + endIdx - 1;
                std::ostringstream q;
                q << "SELECT * FROM ( " << baseQuery << " ) t WHERE (" << splitColumn
                  << " BETWEEN " << sliceLo << " AND " << sliceHi << ")";
                sliceQueries.push_back(q.str());
            }
        }

        // NULL keys match no BETWEEN/MOD slice; keep them once on the last slice.
        if (!sliceQueries.empty()) {
            sliceQueries.back() += " OR " + splitColumn + " IS NULL";
        }

        if (sliceQueries.empty()) {         // defensive: never leave zero slices
            sliceQueries.push_back(baseQuery);
        }
        srvInterface.log("ODBC Loader: split_column '%s' -> %zu slice(s) using %s method",
                         splitColumn.c_str(), sliceQueries.size(),
                         useModulo ? "modulo" : "range");
    }

    // Runs the next slice on the existing statement; column bindings persist.
    // Single-slice path only (thread_count<=1 / no split / unusable split column).
    void executeSlice(ServerInterface &srvInterface, const std::string &sliceQuery) {
        SQLRETURN r = SQLFreeStmt(stmt, SQL_CLOSE);
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLFreeStmt(SQL_CLOSE)");
        r = SQLExecDirect(stmt, (SQLCHAR*)sliceQuery.c_str(), SQL_NTS);
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLExecDirect()");
    }

    // Converts one already-fetched cell (raw driver bytes) into the writer; main
    // thread only. NOTE: the single-slice loop in process() still has its own copy.
    void emitCell(ServerInterface &srvInterface, SQLUSMALLINT i, SQLPOINTER buf, SQLLEN len) {
        Buf data;
        data.buf = buf;
        data.len = len;
        std::string rejectReason = "unrecognized syntax from remote database";

        switch (vtype[i]) {

            // Simple fixed-length types
            // Let C++ figure out how to convert from, ie., SQLBIGINT to vint.
        case BoolOID:
            writer->setBool(vidx.at(i), (*(SQLCHAR*)data.buf == SQL_TRUE ? VTrue : VFalse));
            break;
        case Int8OID:
            if (quirks != Oracle) {
                writer->setInt(vidx.at(i), *(SQLBIGINT*)data.buf);
            } else {
                // Oracle doesn't support int64 as a type.
                // So we get the data as a string and parse it to an int64.
                if (data.len == SQL_NTS) { writer->setInt(vidx.at(i), vint_null); }
                else { writer->setInt(vidx.at(i), (vint)atoll((char*)data.buf)); }
            }
            break;
        case Float8OID:
            writer->setFloat(vidx.at(i), *(SQLDOUBLE*)data.buf);
            break;
        case CharOID: case BinaryOID:
        case VarcharOID: case VarbinaryOID:
#ifndef NO_LONG_OIDS
        case LongVarcharOID: case LongVarbinaryOID:
#endif
            if (data.len == SQL_NTS) {
                data.len = strnlen((char*)data.buf, getFieldSizeForCol(vidx.at(i)));
            }
            writer->getStringRef(vidx.at(i)).copy((char*)data.buf, data.len);
            break;

            // Date/Time functions that work in reasonably direct ways
        case DateOID: {
            SQL_DATE_STRUCT &s = *(SQL_DATE_STRUCT*)data.buf;
            struct tm d = {0,0,0,s.day,s.month-1,s.year-1900,0,0,-1};
            time_t unixtime = mktime(&d);
            writer->setDate(vidx.at(i), getDateFromUnixTime(unixtime + d.tm_gmtoff));
            break;
        }
        case TimeOID: {
            SQL_TIME_STRUCT &s = *(SQL_TIME_STRUCT*)data.buf;
            writer->setTime(vidx.at(i), getTimeFromHMS(s.hour, s.minute, s.second));
            break;
        }
        case TimestampOID: {
            SQL_TIMESTAMP_STRUCT &s = *(SQL_TIMESTAMP_STRUCT*)data.buf;
            struct tm d = {s.second,s.minute,s.hour,s.day,s.month-1,s.year-1900,0,0,-1};
            time_t unixtime = mktime(&d);
            // s.fraction is in nanoseconds; Vertica only does microsecond resolution
            writer->setTimestamp(vidx.at(i), getTimestampFromUnixTime(unixtime + d.tm_gmtoff) + s.fraction/1000);
            break;
        }

            // Date/Time functions that require string-parsing
        case TimeTzOID: {
            // Hacky workaround:  Some databases (ie., us) send the empty string instead of NULL here
            if (((char*)data.buf)[0] == '\0') { writer->setNull(vidx.at(i)); break; }
            TimeADT t = 0;
            if (!parser.parseTimeTz((char*)data.buf, (size_t)data.len, i, t, getVerticaTypeOfCol(vidx.at(i)), rejectReason)) {
                vt_report_error(0, "Error parsing TimeTz: '%s' (%s)", (char*)data.buf, rejectReason.c_str());
            }
            writer->setTimeTz(vidx.at(i),t);
            break;
        }

        case TimestampTzOID: {
            // Hacky workaround:  Some databases (ie., us) send the empty string instead of NULL here
            if (((char*)data.buf)[0] == '\0') { writer->setNull(vidx.at(i)); break; }
            TimestampTz t = 0;
            if (!parser.parseTimestampTz((char*)data.buf, (size_t)data.len, i, t, getVerticaTypeOfCol(vidx.at(i)), rejectReason)) {
                vt_report_error(0, "Error parsing TimestampTz: '%s' (%s)", (char*)data.buf, rejectReason.c_str());
            }
            writer->setTimestampTz(vidx.at(i),t);
            break;
        }

        case IntervalOID: {
            SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)data.buf;
            if (intv.interval_type != SQL_IS_DAY_TO_SECOND) {
                vt_report_error(0, "Error parsing Interval:  Is type %d; expecting type 10 (SQL_IS_HOUR_TO_SECOND)", (int)intv.interval_type);
            }
            // Vertica Intervals are stored as durations in microseconds
            Interval ret = ((intv.intval.day_second.day*usPerDay)
                            + (intv.intval.day_second.hour*usPerHour)
                            + (intv.intval.day_second.minute*usPerMinute)
                            + (intv.intval.day_second.second*usPerSecond)
                            + (intv.intval.day_second.fraction/1000)) // Fractions are in nanoseconds; we do microseconds
                * (intv.interval_sign == SQL_TRUE ? -1 : 1); // Apply the sign bit
            writer->setInterval(vidx.at(i), ret);
            break;
        }

        case IntervalYMOID: {
            SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)data.buf;
            if (intv.interval_type != SQL_IS_YEAR_TO_MONTH) {
                vt_report_error(0, "Error parsing Interval:  Is type %d; expecting type 7 (SQL_IS_YEAR_TO_MONTH)", (int)intv.interval_type);
            }
            // Vertica Intervals are stored as durations in months
            Interval ret = ((intv.intval.year_month.year*MONTHS_PER_YEAR)
                            + (intv.intval.year_month.month))
                * (intv.interval_sign == SQL_TRUE ? -1 : 1); // Apply the sign bit
            writer->setInterval(vidx.at(i), ret);
            break;
        }

            // TODO:  Sort out the binary ODBC Numeric format
        case NumericOID: {
            // Hacky workaround:  Some databases may send the empty string instead of NULL here
            if (((char*)data.buf)[0] == '\0') { writer->setNull(vidx.at(i)); break; }
            if (!parser.parseNumeric((char*)data.buf, (size_t)data.len, i, writer->getNumericRef(vidx.at(i)), getVerticaTypeOfCol(vidx.at(i)), rejectReason)) {
                vt_report_error(0, "Error parsing Numeric: '%s' (%s)", (char*)data.buf, rejectReason.c_str());
            }
            break;
        }

        default:
            vt_report_error(0, "Unrecognized Vertica type %s (OID %llu)",
                getVerticaTypeOfCol(vidx.at(i)).getTypeStr(),
                getVerticaTypeOfCol(vidx.at(i)).getTypeOid());
        } // End SWITCH
    }

    // Captures a worker's ODBC failure as data for the main thread to re-raise.
    // No lock: each worker writes only its own slot, read after joinWorkers().
    void captureWorkerError(int workerIdx, SQLSMALLINT handleType, SQLHANDLE handle,
                            const char *fnName) {
        SQLCHAR state_rec[6] = {0};
        SQLINTEGER native_code = 0;
        SQLCHAR message_text[MAX_DIAG_MSG_TEXT_LENGTH] = {0};
        SQLSMALLINT msg_length = 0;
        SQLGetDiagRec(handleType, handle, 1, &state_rec[0], &native_code,
                      &message_text[0], MAX_DIAG_MSG_TEXT_LENGTH, &msg_length);
        WorkerStatus &ws = workerStatus[workerIdx];
        ws.failed = true;
        ws.sqlstate = std::string((char*)state_rec);
        std::ostringstream m;
        m << fnName << " failed [" << (char*)message_text << "] (native " << (int)native_code << ")";
        ws.message = m.str();
    }

    // Worker entry point: owns its own env/dbc/stmt and buffers, and shares only
    // immutable metadata. Takes no ServerInterface so it cannot touch the writer.
    void workerRun(int workerIdx, std::string sliceQuery) {
        SQLHENV wenv = SQL_NULL_HENV;
        SQLHDBC wdbc = SQL_NULL_HDBC;
        SQLHSTMT wstmt = SQL_NULL_HSTMT;
        std::vector<SQLPOINTER> wresp(numcols, (SQLPOINTER)0);
        std::vector<SQLLEN*> wlenp(numcols, (SQLLEN*)0);
        SQLULEN wnfrows = 0;

        // Local RAII-ish cleanup: free everything this worker owns on every path.
        struct Cleanup {
            SQLHENV *e; SQLHDBC *d; SQLHSTMT *s;
            std::vector<SQLPOINTER> *rp; std::vector<SQLLEN*> *lp;
            ~Cleanup() {
                for (size_t c = 0; rp && c < rp->size(); c++) free((*rp)[c]);
                for (size_t c = 0; lp && c < lp->size(); c++) free((*lp)[c]);
                if (s && *s != SQL_NULL_HSTMT) { SQLFreeStmt(*s, SQL_CLOSE); SQLFreeHandle(SQL_HANDLE_STMT, *s); }
                if (d && *d != SQL_NULL_HDBC) { SQLDisconnect(*d); SQLFreeHandle(SQL_HANDLE_DBC, *d); }
                if (e && *e != SQL_NULL_HENV) { SQLFreeHandle(SQL_HANDLE_ENV, *e); }
            }
        } cleanup = { &wenv, &wdbc, &wstmt, &wresp, &wlenp };

        bool done = false;
        // unixODBC serializes connections unless the driver sets Threading = 0; see README.
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &wenv)) ||
            !SQL_SUCCEEDED(SQLSetEnvAttr(wenv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0)) ||
            !SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, wenv, &wdbc))) {
            captureWorkerError(workerIdx, SQL_HANDLE_ENV, wenv, "SQLAllocHandle(worker env/dbc)");
            done = true;
        }
        if (!done && !SQL_SUCCEEDED(SQLDriverConnect(wdbc, NULL, (SQLCHAR*)connect.c_str(),
                                                     SQL_NTS, NULL, 0, NULL, SQL_DRIVER_COMPLETE))) {
            captureWorkerError(workerIdx, SQL_HANDLE_DBC, wdbc, "SQLDriverConnect(worker)");
            done = true;
        }
        if (!done && !SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, wdbc, &wstmt))) {
            captureWorkerError(workerIdx, SQL_HANDLE_DBC, wdbc, "SQLAllocHandle(worker stmt)");
            done = true;
        }
        if (!done) {
            SQLSetStmtAttr(wstmt, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0);
            SQLSetStmtAttr(wstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)rowset, 0);
            SQLSetStmtAttr(wstmt, SQL_ATTR_ROWS_FETCHED_PTR, &wnfrows, 0);
            if (!SQL_SUCCEEDED(SQLExecDirect(wstmt, (SQLCHAR*)sliceQuery.c_str(), SQL_NTS))) {
                captureWorkerError(workerIdx, SQL_HANDLE_STMT, wstmt, "SQLExecDirect(worker)");
                done = true;
            }
        }
        // Same column bindings scheme as the shared single-connection fetch.
        for (SQLSMALLINT i = 0; !done && i < numcols; i++) {
            wresp[i] = (SQLPOINTER)malloc((size_t)stype[i] * rowset);
            wlenp[i] = (SQLLEN*)malloc(sizeof(SQLLEN) * rowset);
            if (!wresp[i] || !wlenp[i] ||
                !SQL_SUCCEEDED(SQLBindCol(wstmt, i+1, ctype[i],
                                          wresp[i], stype[i], wlenp[i]))) {
                captureWorkerError(workerIdx, SQL_HANDLE_STMT, wstmt, "SQLBindCol(worker)");
                done = true;
            }
        }

        SQLRETURN fetchRet = SQL_SUCCESS;
        while (!done && !workerStatus[workerIdx].failed) {
            if (isCanceled()) break;                            // US 5598915: prompt exit on cancel
            {
                std::unique_lock<std::mutex> lk(queue.mtx);
                if (queue.shutdown) break;
            }
            fetchRet = SQLFetch(wstmt);
            if (!SQL_SUCCEEDED(fetchRet)) break;

            Batch batch;
            batch.rows.reserve((size_t)wnfrows);
            for (uint32 j = 0; j < (uint32)wnfrows; j++) {
                std::vector<Cell> row(numcols);
                for (SQLUSMALLINT i = 0; i < numcols; i++) {
                    SQLLEN len = wlenp[i][j];
                    if ((int)len == (int)SQL_NULL_DATA) {
                        row[i].isNull = true;
                    } else {
                        row[i].isNull = false;
                        // A negative indicator (SQL_NTS/SQL_NO_TOTAL) gives no length, so
                        // copy the whole field; emitCell() re-measures it on the main thread.
                        size_t n = (len == SQL_NTS || len < 0) ? (size_t)stype[i] : (size_t)len;
                        if (n > (size_t)stype[i]) n = (size_t)stype[i];
                        row[i].bytes.assign((char*)wresp[i] + (size_t)stype[i]*j, n);
                        row[i].lenIndicator = len;
                    }
                }
                batch.rows.push_back(std::move(row));
            }

            // Blocks here when the queue is full; this is the backpressure.
            std::unique_lock<std::mutex> lk(queue.mtx);
            queue.notFull.wait(lk, [this]{
                return queue.items.size() < queue.maxItems || queue.shutdown;
            });
            if (queue.shutdown) break;                          // woke to exit
            queue.items.push_back(std::move(batch));
            queue.notEmpty.notify_one();
        }

        if (!done && !SQL_SUCCEEDED(fetchRet) && fetchRet != SQL_NO_DATA &&
            !workerStatus[workerIdx].failed && !isCanceled()) {
            captureWorkerError(workerIdx, SQL_HANDLE_STMT, wstmt, "SQLFetch(worker)");
        }

        // Last producer out must wake a consumer that is waiting on an empty queue.
        {
            std::unique_lock<std::mutex> lk(queue.mtx);
            queue.activeProducers--;
            queue.notEmpty.notify_all();
            queue.notFull.notify_all();
        }
    }

    // Spawns up to min(slices, thread_count, MAX_THREAD) workers exactly once.
    void startWorkers() {
        int n = std::min({(int)sliceQueries.size(), threadCountParam, MAX_THREAD});
        workerStatus.assign(n, WorkerStatus());
        queue.activeProducers = n;
        queue.shutdown = false;
        for (int k = 0; k < n; k++) {
            try {
                workers.push_back(std::thread(&ODBCLoader::workerRun, this, k, sliceQueries[k]));
            } catch (...) {
                // Discount producers we never started, or activeProducers never
                // reaches 0 and the consumer waits for a drain that cannot happen.
                std::unique_lock<std::mutex> lk(queue.mtx);
                for (int u = k; u < n; u++) {
                    workerStatus[u].failed = true;
                    workerStatus[u].message = "worker thread creation failed";
                }
                queue.activeProducers -= (n - k);
                queue.notEmpty.notify_all();
                break;
            }
        }
    }

    // Joins ALL workers before returning; never detach(). Signals shutdown so a
    // worker blocked on a full queue wakes and exits (no deadlock).
    void joinWorkers() {
        {
            std::unique_lock<std::mutex> lk(queue.mtx);
            queue.shutdown = true;
            queue.notFull.notify_all();
            queue.notEmpty.notify_all();
        }
        for (size_t k = 0; k < workers.size(); k++) {
            if (workers[k].joinable()) workers[k].join();
        }
        workers.clear();
    }

    // Re-raises the first worker failure on the MAIN thread (US 5602339).
    void raiseFirstWorkerError(ServerInterface &srvInterface) {
        for (size_t k = 0; k < workerStatus.size(); k++) {
            if (workerStatus[k].failed) {
                vt_report_error(0, "ODBC Loader worker error: %s SQLSTATE=%s",
                                workerStatus[k].message.c_str(),
                                workerStatus[k].sqlstate.c_str());
            }
        }
    }

    bool anyWorkerFailed() const {
        for (size_t k = 0; k < workerStatus.size(); k++) {
            if (workerStatus[k].failed) return true;
        }
        return false;
    }


public:

    virtual StreamState process(ServerInterface &srvInterface, DataBuffer &input, InputState input_state) {
        // Threaded path: the main thread is the sole consumer. Emit a bounded number
        // of batches per call so the KEEP_GOING re-entry contract still holds.
        if (threaded) {
            if (!workersStarted) {
                startWorkers();
                workersStarted = true;
            }

            uint32 batches_emitted = 0;
            while (batches_emitted < BATCHES_PER_BREAK) {
                Batch batch;
                bool haveBatch = false;
                {
                    std::unique_lock<std::mutex> lk(queue.mtx);
                    queue.notEmpty.wait(lk, [this]{
                        return !queue.items.empty() || queue.activeProducers == 0 || queue.shutdown;
                    });
                    if (!queue.items.empty()) {
                        batch = std::move(queue.items.front());
                        queue.items.pop_front();
                        queue.notFull.notify_one();
                        haveBatch = true;
                    }
                }

                if (haveBatch) {
                    for (size_t r = 0; r < batch.rows.size(); r++) {
                        std::vector<Cell> &row = batch.rows[r];
                        for (SQLUSMALLINT i = 0; i < colInTable; i++)
                            writer->setNull(i);                 // set all cols to NULL
                        for (SQLUSMALLINT i = 0; i < numcols; i++) {
                            if (row[i].isNull) continue;
                            emitCell(srvInterface, i, (SQLPOINTER)row[i].bytes.data(), row[i].lenIndicator);
                        }
                        writer->next();
                    }
                    batches_emitted++;
                    continue;
                }

                // Nothing queued: producers are finished, or we were asked to stop.
                break;
            }

            // Cancellation check while draining (US 5598915).
            if (isCanceled()) {
                joinWorkers();
                return DONE;
            }

            // Not done until ALL producers finished AND the queue is fully drained.
            bool drainedAndDone;
            {
                std::unique_lock<std::mutex> lk(queue.mtx);
                drainedAndDone = (queue.activeProducers == 0 && queue.items.empty());
            }
            if (!drainedAndDone) {
                return KEEP_GOING;
            }

            // All workers finished and queue drained: join before returning.
            joinWorkers();
            if (anyWorkerFailed()) {
                // Rows already emitted stay uncommitted: vt_report_error aborts the
                // COPY and Vertica rolls the transaction back.
                raiseFirstWorkerError(srvInterface);
            }
            return DONE;
        }

        // ---- Single-slice path (unchanged) ----
        // Every so many iterations we want to
        // break out and check for Vertica cancel messages
        uint32 iter_counter = 0;

        SQLRETURN fetchRet;
        while (SQL_SUCCEEDED(fetchRet = SQLFetch(stmt))) {
#if LOADER_DEBUG
  srvInterface.log("DEBUG Number of fetched rows/columns = %lu/%d", nfrows, numcols);
#endif
          for (uint32 j = 0; j < (uint32)nfrows; j++) {			// for each fetched row...
            for (SQLUSMALLINT i = 0; i < colInTable; i++) 
                writer->setNull(i);                             // set all cols to NULL
            for (SQLUSMALLINT i = 0; i < numcols; i++) {        // for each column...
#if LOADER_DEBUG
  srvInterface.log("DEBUG nfrows=%u j=%u i=%d lenp[%d][%d]=%ld", (uint32)nfrows, j, i, i, j, lenp[i][j]);
#endif

                // MF allocate & set Buf struct so we can re-use the original code in the Fetch loop...
                Buf data ;

                // MF SQLPOINTER is a (void *) so it would generate an arithmetic warning if not casted
                data.buf = (SQLPOINTER)( (uint8_t *)resp[i] + stype[i] * j ) ;
                data.len = lenp[i][j] ;

                std::string rejectReason = "unrecognized syntax from remote database";
                
                if ((int)data.len != (int)SQL_NULL_DATA ) {     // (re)write NOT NULL cols
                    switch (vtype[i]) {
                        
                        // Simple fixed-length types
                        // Let C++ figure out how to convert from, ie., SQLBIGINT to vint.
                        // (Both are native C++ types with appropriate meanings, so hopefully this will DTRT.)
                        // (In most implementations they are probably the same type so this is a no-op.)
                    case BoolOID:
                        writer->setBool(vidx.at(i), (*(SQLCHAR*)data.buf == SQL_TRUE ? VTrue : VFalse));
                        break;
                    case Int8OID:
                        if (quirks != Oracle) {
                            writer->setInt(vidx.at(i), *(SQLBIGINT*)data.buf);
                        } else {
                            // Oracle doesn't support int64 as a type.
                            // So we get the data as a string and parse it to an int64.
                            if (data.len == SQL_NTS) { writer->setInt(vidx.at(i), vint_null); }
                            else { writer->setInt(vidx.at(i), (vint)atoll((char*)data.buf)); }
                        } 
                        break;
                    case Float8OID:
                        writer->setFloat(vidx.at(i), *(SQLDOUBLE*)data.buf); 
                        break;
                    case CharOID: case BinaryOID:
                    case VarcharOID: case VarbinaryOID:
#ifndef NO_LONG_OIDS
                    case LongVarcharOID: case LongVarbinaryOID:
#endif
                        if (data.len == SQL_NTS) { 
                            data.len = strnlen((char*)data.buf, getFieldSizeForCol(vidx.at(i))); 
                        }
                        writer->getStringRef(vidx.at(i)).copy((char*)data.buf, data.len);
                        break;

                        // Date/Time functions that work in reasonably direct ways
                    case DateOID: {
                        SQL_DATE_STRUCT &s = *(SQL_DATE_STRUCT*)data.buf;
                        struct tm d = {0,0,0,s.day,s.month-1,s.year-1900,0,0,-1};
                        time_t unixtime = mktime(&d);
                        writer->setDate(vidx.at(i), getDateFromUnixTime(unixtime + d.tm_gmtoff));
                        break;
                    }
                    case TimeOID: {
                        SQL_TIME_STRUCT &s = *(SQL_TIME_STRUCT*)data.buf;
                        writer->setTime(vidx.at(i), getTimeFromHMS(s.hour, s.minute, s.second));
                        break;
                    }
                    case TimestampOID: {
                        SQL_TIMESTAMP_STRUCT &s = *(SQL_TIMESTAMP_STRUCT*)data.buf;
                        struct tm d = {s.second,s.minute,s.hour,s.day,s.month-1,s.year-1900,0,0,-1};
                        time_t unixtime = mktime(&d);
                        // s.fraction is in nanoseconds; Vertica only does microsecond resolution
                        // setTimestamp() wants time since epoch localtime.
                        writer->setTimestamp(vidx.at(i), getTimestampFromUnixTime(unixtime + d.tm_gmtoff) + s.fraction/1000);
                        break;
                    }
                        
                        // Date/Time functions that require string-parsing
                    case TimeTzOID: {
                        // Hacky workaround:  Some databases (ie., us) send the empty string instead of NULL here
                        if (((char*)data.buf)[0] == '\0') { writer->setNull(vidx.at(i)); break; }
                        TimeADT t = 0;
                        
                        if (!parser.parseTimeTz((char*)data.buf, (size_t)data.len, i, t, getVerticaTypeOfCol(vidx.at(i)), rejectReason)) {
                            vt_report_error(0, "Error parsing TimeTz: '%s' (%s)", (char*)data.buf, rejectReason.c_str());  // No rejected-rows for us!  Die on failure.
                        }
                        writer->setTimeTz(vidx.at(i),t);
                        break;
                    }
                        
                    case TimestampTzOID: {
                        // Hacky workaround:  Some databases (ie., us) send the empty string instead of NULL here
                        if (((char*)data.buf)[0] == '\0') { writer->setNull(vidx.at(i)); break; }
                        TimestampTz t = 0;
                        if (!parser.parseTimestampTz((char*)data.buf, (size_t)data.len, i, t, getVerticaTypeOfCol(vidx.at(i)), rejectReason)) {
                            vt_report_error(0, "Error parsing TimestampTz: '%s' (%s)", (char*)data.buf, rejectReason.c_str());  // No rejected-rows for us!  Die on failure.
                        }
                        writer->setTimestampTz(vidx.at(i),t);
                        break;
                    }
                        
                    case IntervalOID: {
                        SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)data.buf;
                        
                        // Make sure we know what we're talking about
                        if (intv.interval_type != SQL_IS_DAY_TO_SECOND) {
                            vt_report_error(0, "Error parsing Interval:  Is type %d; expecting type 10 (SQL_IS_HOUR_TO_SECOND)", (int)intv.interval_type);
                        }

                        // Vertica Intervals are stored as durations in microseconds
                        Interval ret = ((intv.intval.day_second.day*usPerDay)
                                        + (intv.intval.day_second.hour*usPerHour)
                                        + (intv.intval.day_second.minute*usPerMinute)
                                        + (intv.intval.day_second.second*usPerSecond)
                                        + (intv.intval.day_second.fraction/1000)) // Fractions are in nanoseconds; we do microseconds
                            * (intv.interval_sign == SQL_TRUE ? -1 : 1); // Apply the sign bit
                        
                        writer->setInterval(vidx.at(i), ret);
                        break;   
                    }

                    case IntervalYMOID: {
                        SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)data.buf;
                        
                        // Make sure we know what we're talking about
                        if (intv.interval_type != SQL_IS_YEAR_TO_MONTH) {
                            vt_report_error(0, "Error parsing Interval:  Is type %d; expecting type 7 (SQL_IS_YEAR_TO_MONTH)", (int)intv.interval_type);
                        }

                        // Vertica Intervals are stored as durations in months
                        Interval ret = ((intv.intval.year_month.year*MONTHS_PER_YEAR)
                                        + (intv.intval.year_month.month))
                            * (intv.interval_sign == SQL_TRUE ? -1 : 1); // Apply the sign bit
                        
                        writer->setInterval(vidx.at(i), ret);
                        break;   
                    }
                        
                        // TODO:  Sort out the binary ODBC Numeric format
                        // and the abilities of various DB's to cast to/from it on demand;
                        // make this use the native binary format and cast/convert as needed.
                    case NumericOID: {
                        // Hacky workaround:  Some databases may send the empty string instead of NULL here
                        if (((char*)data.buf)[0] == '\0') { writer->setNull(vidx.at(i)); break; }
                        if (!parser.parseNumeric((char*)data.buf, (size_t)data.len, i, writer->getNumericRef(vidx.at(i)), getVerticaTypeOfCol(vidx.at(i)), rejectReason)) {
                            vt_report_error(0, "Error parsing Numeric: '%s' (%s)", (char*)data.buf, rejectReason.c_str());  // No rejected-rows for us!  Die on failure.
                        }
                        break;
                    }

                    default:
                        vt_report_error(0, "Unrecognized Vertica type %s (OID %llu)",
                            getVerticaTypeOfCol(vidx.at(i)).getTypeStr(), 
                            getVerticaTypeOfCol(vidx.at(i)).getTypeOid());
                } // End SWITCH
              }   // End IF NOT NULL
            }     // End FOR EACH COLUMN

            writer->next();	// avanzamento alla riga successiva (scrive e avanza il cursor)

            if (++iter_counter == ROWS_PER_BREAK) {
                // Periodically yield and let upstream do its thing
                return KEEP_GOING;
            }
          }      // End FOR EACH ROW
        }        // End FETCH LOOP

        // If SQLFetch() failed for some reason, report it
        // But, SQLFetch() is allowed to return SQL_NO_DATA from time to time.
        // TODO:  Maybe be smarter if we're getting SQL_NO_DATA forever / apparently stuck?
        if (fetchRet != SQL_NO_DATA) {
            handleReturnCode(srvInterface, fetchRet, SQL_HANDLE_STMT, stmt, "SQLFetch()");
        }

        // Current slice drained; run the next one if any and keep going, else DONE.
        if (currentSlice + 1 < (int)sliceQueries.size()) {
            executeSlice(srvInterface, sliceQueries[++currentSlice]);
            return KEEP_GOING;
        }

        return DONE;
    }           // End PROCESS

    void setQuirksMode(ServerInterface &srvInterface, SQLHDBC &dbc) {
        // Set the quirks mode based on the DB name
        SQLSMALLINT len;
        char buf[32];
        memset(&buf[0], 0, 32);

        SQLGetInfo(dbc, SQL_SERVER_NAME, buf,
                   sizeof(buf) - 1 /* leave a byte for null-termination */,
                   &len);
        srvInterface.log("ODBC Loader: Connecting to server of type '%s'", buf);

        std::string db_type(buf, len);
        if (db_type == "ORCL") {
            quirks = Oracle;
        }

        // MOD() is engine-dependent (SQL Server/Sybase use %); allow-list known engines.
        // MariaDB reports its own DBMS name but shares MySQL's MOD() sign semantics.
        char dbms[64];
        memset(&dbms[0], 0, sizeof(dbms));
        SQLGetInfo(dbc, SQL_DBMS_NAME, dbms, sizeof(dbms) - 1, NULL);
        std::string dbms_name(dbms);
        for (size_t i = 0; i < dbms_name.size(); i++) {
            char c = dbms_name[i];
            if (c >= 'A' && c <= 'Z') dbms_name[i] = c - 'A' + 'a';
        }
        modSupported = (dbms_name.find("postgres") != std::string::npos ||
                        dbms_name.find("oracle")   != std::string::npos ||
                        dbms_name.find("mysql")    != std::string::npos ||
                        dbms_name.find("mariadb")  != std::string::npos);
    }

    virtual void setup(ServerInterface &srvInterface, SizedColumnTypes &returnType) {
        // Capture our column types
        colInfo = returnType;
		bool src_rfilter = true ;       // Rows filtering flag
        bool src_cfilter = true ;       // Column filtering flag
        bool oq_flag = false ;          // Query Ovverride flag
        // 'connect' is a member so workers can open their own connections.
        connect = "" ;                  // Connect string
        std::string query = "" ;        // Remote system query string
        std::string predicates = "" ;   // Predicates

        // Read User defined Session parameters 
        if (srvInterface.getUDSessionParamReader("library").containsParameter("src_rfilter")) {
            src_rfilter = ( srvInterface.getUDSessionParamReader("library").getStringRef("src_rfilter").str() == "f" ) ? false : true ;
        } else if (srvInterface.getParamReader().containsParameter("src_rfilter")) {
            src_rfilter = srvInterface.getParamReader().getBoolRef("src_rfilter") ;
        }
        if (srvInterface.getUDSessionParamReader("library").containsParameter("override_query")) {
            query = srvInterface.getUDSessionParamReader("library").getStringRef("override_query").str() ;
        } else {
            query = srvInterface.getParamReader().getStringRef("query").str();
        }
        if (srvInterface.getUDSessionParamReader("library").containsParameter("src_cfilter")) {
            src_cfilter = ( srvInterface.getUDSessionParamReader("library").getStringRef("src_cfilter").str() == "f" ) ? false : true ;
        } else if (srvInterface.getParamReader().containsParameter("src_cfilter")) {
            src_cfilter = srvInterface.getParamReader().getBoolRef("src_cfilter") ;
        }
        connect = srvInterface.getParamReader().getStringRef("connect").str();
#if LOADER_DEBUG
  srvInterface.log("DEBUG Initial connect=<%s>", connect.c_str());
  srvInterface.log("DEBUG Initial query=<%s>", query.c_str());
  srvInterface.log("DEBUG SETUP src_rfilter is %s", src_rfilter ? "true" : "false" );
  srvInterface.log("DEBUG SETUP src_cfilter is %s", src_cfilter ? "true" : "false" );
#endif

        // Check Connection string, Query and Rowset "public" parameters

        // Check "rowset" parameter
        if (srvInterface.getParamReader().containsParameter("rowset")) {
            vint rowset_param = srvInterface.getParamReader().getIntRef("rowset") ;
            if ( rowset_param < MIN_ROWSET || rowset_param > MAX_ROWSET ) 
                vt_report_error(0, "Error:  Invalid rowset=%zd. Permitted values between %d and %d", rowset_param, MIN_ROWSET, MAX_ROWSET);
            else
                rowset = (size_t) rowset_param ;
        } else {
                rowset = DEF_ROWSET ;	// use default if not set
        }

        // Check "thread_count" parameter (valid range MIN_THREAD..MAX_THREAD).
        vint thread_count = DEF_THREAD ;
        if (srvInterface.getParamReader().containsParameter("thread_count")) {
            thread_count = srvInterface.getParamReader().getIntRef("thread_count") ;
            if ( thread_count < MIN_THREAD || thread_count > MAX_THREAD )
                vt_report_error(0, "Error:  Invalid thread_count=%zd. Permitted values between %d and %d", thread_count, MIN_THREAD, MAX_THREAD);
        }
        threadCountParam = (int)thread_count ;   // caps the worker count in startWorkers()

        // Check "split_column" (bare identifier) and "split_method" ("range"/"modulo").
        std::string split_column = "" ;
        if (srvInterface.getParamReader().containsParameter("split_column")) {
            split_column = srvInterface.getParamReader().getStringRef("split_column").str() ;
        }
        std::string split_method = "range" ;
        if (srvInterface.getParamReader().containsParameter("split_method")) {
            split_method = srvInterface.getParamReader().getStringRef("split_method").str() ;
            for (size_t i = 0 ; i < split_method.size() ; i++) {
                char c = split_method[i] ;
                if (c >= 'A' && c <= 'Z') split_method[i] = c - 'A' + 'a' ;
            }
        }
  
        // Check "hidden" parameters __pred_#__ to filter out rows
        char pred[16] ;
        for ( unsigned int k = 0, l = 0 ; k < MAX_PRENUM ; k++ ) {
            snprintf(pred, sizeof(pred), "__pred_%u__", k ) ;
            if (srvInterface.getParamReader().containsParameter(pred)) {
                std::string mpred = srvInterface.getParamReader().getStringRef(pred).str() ;
#if LOADER_DEBUG
  srvInterface.log("DEBUG predicate [%s] length=%zu, string=<%s>", pred, strlen(mpred.c_str()), mpred.c_str());
#endif
                {
                    static const std::regex re_query(REG_QUERYP, std::regex::ECMAScript | std::regex::icase);
                    static const std::regex re_any(REG_ANYMTC, std::regex::ECMAScript | std::regex::icase);
                    static const std::regex re_tilde(REG_TILDEM, std::regex::ECMAScript | std::regex::icase);

                    std::smatch m;
                    if (std::regex_match(mpred, m, re_query)) {
                        mpred = m[1].str();
                        query = mpred ;
                        oq_flag = true ;
#if LOADER_DEBUG
  srvInterface.log("DEBUG new query length=%zu, new query string=<%s>",query.length(),  query.c_str());
#endif
                    } else if ( src_rfilter ) {
                        mpred = std::regex_replace(mpred, re_any, REG_ANYREP);     // to replace ANY(ARRAY()) with IN()
                        mpred = std::regex_replace(mpred, re_tilde, REG_TILDER);  // to replace ~~ with LIKE
                        if ( l++ ) 
                            predicates += " AND " + mpred ;
                        else
                            predicates += " WHERE " + mpred ;
                    }
                }
            } else {
                break ;
            }
        }

        // Remove ending semicolon from "query" (if any)
        {
            static const std::regex re_end(REG_ENDSCO, std::regex::ECMAScript);
            query = std::regex_replace(query, re_end, "");
        }

        // Check "hidden" parameters __query_col_name__ and __query_col_idx__ to filter out columns
        if ( src_cfilter ) {
            // Only column-filter when the param is actually non-empty, a plain COPY sends it empty.
            if (srvInterface.getParamReader().containsParameter("__query_col_name__") &&
                !srvInterface.getParamReader().getStringRef("__query_col_name__").str().empty()) {
                if (srvInterface.getParamReader().containsParameter("__query_col_idx__")) {
                    colInTable = (int)colInfo.getColumnCount() ;
#if LOADER_DEBUG
 srvInterface.log("DEBUG __query_col_name__=<%s>",srvInterface.getParamReader().getStringRef("__query_col_name__").str().c_str());
 srvInterface.log("DEBUG __query_col_idx__=<%s>",srvInterface.getParamReader().getStringRef("__query_col_idx__").str().c_str());
srvInterface.log("-----> External Table Columns, colInTable=<%d>", colInTable);
#endif
                   std::string slist=srvInterface.getParamReader().getStringRef("__query_col_name__").str();
                   std::stringstream ss_idx(srvInterface.getParamReader().getStringRef("__query_col_idx__").str());
                   std::string tk_idx ;
                   vidx.clear();
                   while (std::getline(ss_idx, tk_idx, ',')) {
		   	            vidx.push_back(stoi(tk_idx));
                   }

                   // MF to remove Vertica casts (::<data_type>)
                   {
                       static const std::regex re_cast(REG_CASTRM, std::regex::ECMAScript);
                       slist = std::regex_replace(slist, re_cast, "");
                   }
                   query = "SELECT " + slist + " FROM ( " + query + " ) sq" ;
               } else {
                   query = "SELECT " +
                           srvInterface.getParamReader().getStringRef("__query_col_name__").str() +
                           " FROM ( " +
                           query  +
                           " ) sq" ;
               }
            } else {
                // Plain COPY: column-filter param present but empty -> load all columns.
                query = "SELECT * FROM ( " + query + " ) sq" ;
            }

        } else {
            query = oq_flag ? "SELECT '.' AS override_query, sq.* FROM ( " + query + " ) sq" : "SELECT  * FROM ( " + query + " ) sq" ;
        }

        // Append predicates to outer SELECT
        if ( src_rfilter )
            query += predicates ;
  
        SQLRETURN r;
#if LOADER_DEBUG
  srvInterface.log("DEBUG query=%s", query.c_str());
  srvInterface.log("DEBUG rowset=%zu", rowset);
#endif

        // Establish an ODBC connection
        r = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
        handleReturnCode(srvInterface, r, SQL_HANDLE_ENV, env, "SQLAllocHandle()");

        r = SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        handleReturnCode(srvInterface, r, SQL_HANDLE_ENV, env, "SQLSetEnvAttr()");

        r = SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
        handleReturnCode(srvInterface, r, SQL_HANDLE_DBC, dbc, "SQLAllocHandle(SQL_HANDLE_DBC)");

        r = SQLDriverConnect(dbc, NULL, (SQLCHAR*)connect.c_str(), SQL_NTS, NULL, 0, NULL, SQL_DRIVER_COMPLETE);
        handleReturnCode(srvInterface, r, SQL_HANDLE_DBC, dbc, "SQLDriverConnect()");

        // We have a connection; now we know enough to figure out
        // which DB we have to customize to
        setQuirksMode(srvInterface, dbc);

        r = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLAllocHandle(SQL_HANDLE_STMT)");

        // Set bind by column statement attribute:
        r = SQLSetStmtAttr(stmt, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0) ;
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLSetStmtAttr(SQL_ATTR_ROW_BIND_TYPE)");

        // Set ROW_ARRAY_SIZE statement attribute:
        r = SQLSetStmtAttr(stmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)rowset, 0) ;
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLSetStmtAttr(SQL_ATTR_ROW_ARRAY_SIZE)");

        // Set ROWS_FETCHED_PTR statement attribute:
        r = SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &nfrows, 0) ;
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLSetStmtAttr(SQL_ATTR_ROWS_FETCHED_PTR)");

        // Derive the split and run the first/only slice; unsplit path is unchanged.
        buildSliceQueries(srvInterface, query, (int)thread_count, split_column, split_method);
        // Slice 0 also runs below on the shared connection to derive column metadata,
        // so worker 0 re-runs it; harmless for a SELECT, but it is one extra execution.
        threaded = (sliceQueries.size() > 1);
        r = SQLExecDirect(stmt, (SQLCHAR*)sliceQueries[currentSlice].c_str(), SQL_NTS);
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLExecDirect()");

        r = SQLNumResultCols(stmt, &numcols);
        handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLNumResultCols()");

        // Allocate space for result & length array pointers
        resp = (SQLPOINTER *)srvInterface.allocator->alloc(numcols * sizeof(SQLPOINTER)) ;
        lenp = (SQLLEN **)srvInterface.allocator->alloc(numcols * sizeof(SQLLEN *)) ;

        // Allocate space for Vertica data types OID and size
        vtype = (BaseDataOID *)srvInterface.allocator->alloc(numcols * sizeof(BaseDataOID)) ;
        stype = (uint32 *)srvInterface.allocator->alloc(numcols * sizeof(uint32)) ;
        ctype = (SQLSMALLINT *)srvInterface.allocator->alloc(numcols * sizeof(SQLSMALLINT)) ;

        // Plain COPY leaves vidx empty. Default to identity mapping and set
        // colInTable so the pre-null loop covers all columns.
        if (vidx.empty()) {
            for (SQLSMALLINT i = 0; i < numcols; i++) {
                vidx.push_back(i);
            }
            colInTable = numcols;
        }

        // Set up column-data buffers
        // Bind to the columns in question
        for (SQLSMALLINT i = 0; i < numcols; i++) {
            vtype[i] = getVerticaTypeOfCol(vidx.at(i)).getTypeOid();
            stype[i] = getFieldSizeForCol(vidx.at(i)) ;
            ctype[i] = getCTypeOfCol(vidx.at(i)) ;
#if LOADER_DEBUG
  srvInterface.log("DEBUG i=%d rowset=%zu stype[i]=%d", i, rowset, stype[i]);
#endif
            resp[i] = (SQLPOINTER)srvInterface.allocator->alloc(stype[i] * rowset);
            lenp[i] = (SQLLEN *)srvInterface.allocator->alloc(sizeof(SQLLEN) * rowset);

            r = SQLBindCol(stmt, i+1, ctype[i], resp[i], stype[i], lenp[i]);
            handleReturnCode(srvInterface, r, SQL_HANDLE_STMT, stmt, "SQLBindCol()");
        }

        // Workers fetch on their own connections, so the shared cursor is only
        // needed for the metadata derived above.
        if (threaded) {
            SQLFreeStmt(stmt, SQL_CLOSE);
        }
    }

    virtual void destroy(ServerInterface &srvInterface, SizedColumnTypes &returnType) {
        // Join before freeing handles; a joinable thread left at destruction terminates.
        joinWorkers();

        // Fix for Issue #1. Commit before calling SQLDisconnect to avoid HY010 error.
        SQLRETURN r_end_tran = SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        handleReturnCode(srvInterface, r_end_tran, SQL_HANDLE_DBC, dbc, "SQLEndTran()");

        // Try to free even on error, to minimize the risk of memory leaks.
        // But do check for errors in the end.
        SQLRETURN r_disconnect = SQLDisconnect(dbc);
        SQLRETURN r_free_dbc = SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLRETURN r_free_env = SQLFreeHandle(SQL_HANDLE_ENV, env);

        handleReturnCode(srvInterface, r_disconnect, SQL_HANDLE_DBC, dbc, "SQLDisconnect()");
        handleReturnCode(srvInterface, r_free_dbc, SQL_HANDLE_DBC, dbc, "SQLFreeHandle(SQL_HANDLE_DBC)");
        handleReturnCode(srvInterface, r_free_env, SQL_HANDLE_ENV, env, "SQLFreeHandle(SQL_HANDLE_ENV)");
    }
};

class ODBCLoaderFactory : public ParserFactory {
public:
    virtual void plan(ServerInterface &srvInterface,
            PerColumnParamReader &perColumnParamReader,
            PlanContext &planCtxt) {
        if (!srvInterface.getParamReader().containsParameter("connect")) {
            vt_report_error(0, "Error:  ODBCConnect requires a 'connect' string containing ODBC connect information (at minimum, 'DSN=myDSN' for some myDSN in odbc.ini)");
        }
        if (!srvInterface.getParamReader().containsParameter("query")) {
            vt_report_error(0, "Error:  ODBCConnect requires a 'query' string, the query to execute on the remote system");
        }
    }

    virtual UDParser* prepare(ServerInterface &srvInterface,
            PerColumnParamReader &perColumnParamReader,
            PlanContext &planCtxt,
            const SizedColumnTypes &returnType)
    {
        return vt_createFuncObj(srvInterface.allocator, ODBCLoader);
    }

    virtual void getParameterType(ServerInterface &srvInterface,
                                  SizedColumnTypes &parameterTypes) {
        parameterTypes.addVarchar(65000, "connect");
        parameterTypes.addVarchar(65000, "query");
        parameterTypes.addVarchar(65000, "__query_col_name__");
        parameterTypes.addVarchar(65000, "__query_col_idx__");
        char pred[16] ;
	    for ( unsigned int k = 0 ; k < MAX_PRENUM ; k++ ) {
                snprintf(pred, sizeof(pred), "__pred_%u__", k ) ;
                parameterTypes.addVarchar(MAX_PRELEN, pred);
        }
        parameterTypes.addInt("rowset");
        parameterTypes.addInt("thread_count");
        parameterTypes.addVarchar(128, "split_column");
        parameterTypes.addVarchar(16, "split_method");
        parameterTypes.addBool("src_rfilter");
        parameterTypes.addBool("src_cfilter");
    }
};

RegisterFactory(ODBCLoaderFactory);


// ODBCLoader does all the real work.
// This is basically a stub that tells Vertica to run the query on the current node only.
class ODBCSource : public UDSource {
public:
    virtual StreamState process(ServerInterface &srvInterface, DataBuffer &output) {
        if (output.size < 1) return OUTPUT_NEEDED;
        output.offset = 1;
        return DONE;
    }
};

class ODBCSourceFactory : public SourceFactory {
public:

    virtual void plan(ServerInterface &srvInterface,
            NodeSpecifyingPlanContext &planCtxt) {
        // Make the query only run on the current node.
        std::vector<std::string> nodes;
        nodes.push_back(srvInterface.getCurrentNodeName());
        planCtxt.setTargetNodes(nodes);
    }


    virtual std::vector<UDSource*> prepareUDSources(ServerInterface &srvInterface,
            NodeSpecifyingPlanContext &planCtxt) {
        std::vector<UDSource*> retVal;
        retVal.push_back(vt_createFuncObj(srvInterface.allocator, ODBCSource));
        return retVal;
    }
};
RegisterFactory(ODBCSourceFactory);

// Library Metadata
RegisterLibrary (
    "Vertica Team",
    __DATE__,
    "0.10.6",
    "v11.x.x",
    "TBD",
    "With this loader Vertica can COPY and SELECT from any ODBC data source",
    "", 
    ""  
);      
