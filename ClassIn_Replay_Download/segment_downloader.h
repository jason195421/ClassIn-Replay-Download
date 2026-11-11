#pragma once
// Parallel segment downloader for ClassIn-Replay-Download.
//
// Design is modelled on Motrix-i's SegmentDownloader (flat ordered job list
// with fixed concurrency, per-job retries, byte-accurate progress and cancel
// drain) and on aria2's split / min-split-size / max-connection-per-server
// model (concurrent HTTP Range requests against one large file).
//
// Everything is implemented with WinHTTP + std::thread inside this module so
// the project stays a self-contained exe: no aria2 binary, no Electron, no
// OpenSSL. WinHTTP does TLS through SChannel, which matters because the
// bundled cpp-httplib is compiled without CPPHTTPLIB_OPENSSL_SUPPORT.
//
// Flow: probe URL -> build an ordered job list -> fetch every part with a
// fixed-size worker pool -> concatenate the ordered parts into one local .ts
// (or one merged media file for Range mode) -> hand that local file to the
// existing ffmpeg remux() unchanged.
// If anything at all fails, used_parallel is false and the caller MUST fall
// back to the original single-threaded remux() on the original URL. The
// feature is therefore strictly additive: it can never make things worse.

#include <cstdint>
#include <functional>
#include <string>

// ASCII diagnostic message (never parsed, safe for DownloadStatus.err_msg).
using SegmentLog = std::function<void(const std::string&)>;
// Normalised progress in [0.0, 1.0].
using SegmentProgress = std::function<void(double)>;
// Returns true as soon as the user cancelled (or the run should be aborted).
using SegmentCancel = std::function<bool()>;

struct SegmentOptions {
    int threads = 8;               // parallel part fetches (Motrix-i default 16)
    int tries = 5;                 // network attempts per part (aria2 --max-tries)
    int self_retries = 3;          // extra re-queue attempts (Motrix-i MAX_SELF_RETRIES)
    int retry_wait_ms = 3000;      // aria2 --retry-wait, seconds -> ms
    int connect_timeout_ms = 15000;
    int receive_timeout_ms = 30000;
    int64_t range_min_split_size = 4 * 1024 * 1024; // aria2 --min-split-size
    // Single-file HTTP Range concurrency is OFF by default. For a plain .mp4
    // served by a CDN, ffmpeg streaming the URL directly is faster: parallel
    // Range parts must first be downloaded, then concatenated, then remuxed,
    // which means several full passes over the data and no output until the
    // very end. HLS playlists (many small .ts parts) still benefit from the
    // parallel fetch. Set > 1 (--split N) to opt in.
    int range_splits = 0;         // aria2 --split / --max-connection-per-server
    std::string user_agent = "ClassIn-Replay-Download/1.0";
    std::string referer;
    std::string cookie;
};

struct SegmentResult {
    bool used_parallel = false;    // false => caller must fall back to remux(url)
    bool cancelled = false;        // true  => caller must NOT start remux at all
    std::string error;             // ASCII reason, logged by the caller
    std::string local_input;       // UTF-8 path of the merged local media file
    std::string temp_dir;          // caller must CleanupTempDir() after remux
    int parts = 0;
    int threads = 0;
    double progress = 0.0;
};

// Download one URL into a private temp directory and return a local input
// file for remux(). On ANY problem (master playlist, unsupported encryption,
// HTTP failure, no Range support, cancellation, ...) used_parallel is false.
SegmentResult DownloadSegments(const std::string& url,
                               const SegmentOptions& options,
                               const SegmentCancel& is_cancelled,
                               SegmentProgress on_progress,
                               SegmentLog on_log);

// Best-effort recursive cleanup of a temp directory (never throws).
void CleanupTempDir(const std::string& dir);
