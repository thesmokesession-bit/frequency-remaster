#pragma once

// The log, its history, crash reports, and sending a report (host/core/logs.cpp). The title folder
// (/app0 on the PS5, the settings file's folder on Linux) has frequency.log for this run and logs/
// with the logs of earlier runs (frequency-<date>-<time>.log) and crash reports
// (crash-<date>-<time>.txt), the newest 10 of each.

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Move the last run's frequency.log in pszFolder into logs/, delete the oldest logs and crash
 * reports past 10 of each, and create an empty frequency.log. Runs at start-up only.
 *
 * @return The new log's descriptor (append mode), or -1.
 */
int HostLogOpen(const char *pszFolder);

/**
 * Linux: frequency.log beside the settings file. Standard output and standard error still reach
 * the terminal, and a child process copies both into the log. Then HostReportsStart(). From main().
 */
void HostLogStart(void);

/**
 * Install the crash handler. A crash writes its CRASH lines to nLog and the full report (build,
 * signal, registers, stack, and the end of the log) to logs/crash-<start date>-<time>.txt. Also
 * writes a random FREQ_REPORT_TOPIC to the settings file when it is unset, shows a notification
 * when the last run left a crash report, and crashes at once when FREQ_TEST_CRASH=1. After
 * HostLogOpen() and the settings.
 */
void HostReportsStart(int nLog);

/**
 * Send a report to the ntfy topic FREQ_REPORT_TOPIC on FREQ_REPORT_SERVER (default
 * https://ntfy.sh): the last run's crash report when it left one, otherwise this run's log,
 * trimmed to 24000 bytes with tokens and codes removed. The upload runs on a thread of its own and
 * a notification reports the result. The calling thread does not write files.
 *
 * @return 0 when the upload started, -1 while an earlier one is still running.
 */
int HostSendReport(void);

/** FREQ_SELFTEST=report: the upload of HostSendReport() on this thread. Returns 0 when sent. */
int HostReportSelfTest(void);

#ifdef __cplusplus
}
#endif
