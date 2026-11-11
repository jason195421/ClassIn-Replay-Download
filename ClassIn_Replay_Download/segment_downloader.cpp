#include "segment_downloader.h"

// WinHTTP supplies HTTPS through SChannel. This is essential here: the
// bundled cpp-httplib is compiled WITHOUT CPPHTTPLIB_OPENSSL_SUPPORT, so it
// cannot open https:// URLs at all.
#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// FFmpeg public headers are C headers. They MUST be wrapped in extern "C" or
// the linker will look for C++-mangled av_aes_* / av_free symbols.
extern "C" {
#include <libavutil/aes.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <set>
#include <mutex>
#include <map>
#include <deque>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

const size_t kMaxSegmentBytes = 256u * 1024u * 1024u; // per HLS segment cap
const size_t kProbeBytes = 65536u;                      // header probe
const size_t kMaxPlaylistBytes = 8u * 1024u * 1024u;   // full playlist cap
const int kProgressIntervalMs = 120;

// ---------------------------------------------------------------------------
// Small string / filesystem helpers (all paths are UTF-8 on the wire, widened
// before hitting the Win32 API, exactly like the original utf8_w() helper).
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string Lower(const std::string& s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) r[i] = (char)tolower((unsigned char)r[i]);
    return r;
}

std::string Upper(const std::string& s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) r[i] = (char)toupper((unsigned char)r[i]);
    return r;
}

bool StartsWith(const std::string& s, const std::string& p)
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

uint64_t ParseUInt(const std::string& s)
{
    uint64_t v = 0;
    bool any = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (isdigit((unsigned char)c)) { v = v * 10 + (uint64_t)(c - '0'); any = true; }
        else if (any) break;
    }
    return v;
}

std::string PartName(size_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%06llu.part", (unsigned long long)i);
    return std::string(buf);
}

std::string PadDir(const std::string& dir)
{
    if (dir.empty()) return dir;
    if (dir.back() == '\\' || dir.back() == '/') return dir;
    return dir + "\\";
}

bool WriteBinary(const std::string& path_utf8, const std::string& data)
{
    std::wstring w = Utf8ToWide(path_utf8);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + off, chunk, &wrote, NULL) || wrote != chunk) { ok = false; break; }
        off += wrote;
    }
    CloseHandle(h);
    if (!ok) DeleteFileW(w.c_str());
    return ok;
}

// Append one file to another in bounded chunks. Range parts can be hundreds
// of megabytes, so loading them into a std::string just to concatenate is not
// acceptable; this keeps peak memory flat.
bool AppendFile(const std::string& dst_utf8, const std::string& src_utf8)
{
    std::wstring wsrc = Utf8ToWide(src_utf8);
    std::wstring wdst = Utf8ToWide(dst_utf8);
    HANDLE in = CreateFileW(wsrc.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return false;
    HANDLE out = CreateFileW(wdst.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return false; }
    char buf[1 << 20];
    bool ok = true;
    while (true) {
        DWORD got = 0;
        if (!ReadFile(in, buf, (DWORD)sizeof(buf), &got, NULL)) { ok = false; break; }
        if (got == 0) break;
        DWORD off = 0;
        while (off < got) {
            DWORD wrote = 0;
            if (!WriteFile(out, buf + off, got - off, &wrote, NULL) || wrote == 0) { ok = false; break; }
            off += wrote;
        }
        if (!ok) break;
    }
    CloseHandle(out);
    CloseHandle(in);
    return ok;
}

bool ReadBinary(const std::string& path_utf8, std::string& out)
{
    std::wstring w = Utf8ToWide(path_utf8);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (LONGLONG)kMaxSegmentBytes) {
        CloseHandle(h);
        return false;
    }
    out.clear();
    out.resize((size_t)sz.QuadPart);
    size_t off = 0;
    bool ok = true;
    while (off < out.size()) {
        DWORD want = (DWORD)std::min<size_t>(out.size() - off, 1u << 20);
        DWORD got = 0;
        if (!ReadFile(h, &out[off], want, &got, NULL) || got == 0) { ok = false; break; }
        off += got;
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// ---------------------------------------------------------------------------
// URL handling (hand written: no std::regex here, to stay far away from the
// std::regex_error startup crash this project already hit once).
// ---------------------------------------------------------------------------

struct UrlParts {
    bool valid = false;
    bool https = false;
    std::string host;
    int port = 0;
    std::string path_query;
};

bool ParseUrl(const std::string& url, UrlParts& out)
{
    size_t se = url.find("://");
    if (se == std::string::npos) return false;
    std::string scheme = Lower(url.substr(0, se));
    if (scheme == "https") { out.https = true; out.port = INTERNET_DEFAULT_HTTPS_PORT; }
    else if (scheme == "http") { out.https = false; out.port = INTERNET_DEFAULT_HTTP_PORT; }
    else return false;

    size_t as = se + 3;
    size_t ae = url.find_first_of("/?#", as);
    if (ae == std::string::npos) ae = url.size();
    std::string authority = url.substr(as, ae - as);
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);

    if (!authority.empty() && authority[0] == '[') {
        size_t rb = authority.find(']');
        if (rb == std::string::npos) return false;
        out.host = authority.substr(1, rb - 1);
        if (rb + 1 < authority.size() && authority[rb + 1] == ':')
            out.port = atoi(authority.c_str() + rb + 2);
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            out.port = atoi(authority.c_str() + colon + 1);
        } else out.host = authority;
    }
    if (out.host.empty() || out.port <= 0) return false;

    std::string rest = (ae < url.size()) ? url.substr(ae) : std::string();
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    size_t hash = rest.find('#');
    if (hash != std::string::npos) rest.resize(hash);
    if (rest.empty()) rest = "/";
    out.path_query = rest;
    out.valid = true;
    return true;
}

std::string NormalizePath(const std::string& path)
{
    size_t q = path.find('?');
    std::string p = (q == std::string::npos) ? path : path.substr(0, q);
    std::string suffix = (q == std::string::npos) ? std::string() : path.substr(q);

    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= p.size()) {
        size_t e = p.find('/', start);
        if (e == std::string::npos) { parts.push_back(p.substr(start)); break; }
        parts.push_back(p.substr(start, e - start));
        start = e + 1;
    }
    std::vector<std::string> stack;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& s = parts[i];
        if (s.empty() || s == ".") continue;
        if (s == "..") {
            if (!stack.empty() && stack.back() != "..") stack.pop_back();
            else stack.push_back("..");
            continue;
        }
        stack.push_back(s);
    }
    std::string np = "/";
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i) np += "/";
        np += stack[i];
    }
    if (p.size() > 1 && p.back() == '/' && np.back() != '/') np += '/';
    return np + suffix;
}

std::string ResolveUrl(const std::string& base, const std::string& ref)
{
    std::string low = Lower(ref);
    if (StartsWith(low, "http://") || StartsWith(low, "https://")) return ref;
    if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/') {
        size_t se = base.find("://");
        if (se == std::string::npos) return ref;
        return base.substr(0, se + 1) + ref;
    }
    size_t se = base.find("://");
    if (se == std::string::npos) return ref;
    size_t as = se + 3;
    size_t ae = base.find_first_of("/?#", as);
    if (ae == std::string::npos) ae = base.size();
    std::string origin = base.substr(0, ae);
    std::string rest = (ae < base.size()) ? base.substr(ae) : std::string();
    size_t q = rest.find_first_of("?#");
    if (q != std::string::npos) rest.resize(q);

    std::string full_path;
    if (!ref.empty() && ref[0] == '/') {
        full_path = ref;
    } else {
        size_t ls = rest.rfind('/');
        std::string dir = (ls == std::string::npos) ? std::string("/") : rest.substr(0, ls + 1);
        full_path = dir + ref;
    }
    return origin + NormalizePath(full_path);
}

std::string GuessExtension(const std::string& url)
{
    size_t q = url.find('?');
    std::string p = url.substr(0, q);
    size_t dot = p.rfind('.');
    size_t slash = p.rfind('/');
    if (dot == std::string::npos) return ".mp4";
    if (slash != std::string::npos && dot < slash) return ".mp4";
    std::string e = Lower(p.substr(dot));
    if (e.size() < 2 || e.size() > 6) return ".mp4";
    for (size_t i = 1; i < e.size(); ++i)
        if (!isalnum((unsigned char)e[i])) return ".mp4";
    return e;
}

// ---------------------------------------------------------------------------
// WinHTTP session. One session is created per worker thread and reused across
// parts, so all segments of the same host keep using one keep-alive
// connection instead of paying a fresh TCP + TLS handshake per segment.
// ---------------------------------------------------------------------------

struct HttpResult {
    int status = 0;
    std::string headers;       // raw CRLF header block
    std::string body;          // only filled when stream_path is empty
    size_t bytes = 0;          // total bytes actually read
    bool network_ok = false;   // request was sent and response headers read
    bool io_error = false;
    bool truncated = false;    // part download ended early / hit its byte cap
};

class HttpSession {
public:
    HttpSession()
    {
        session_ = WinHttpOpen(L"ClassIn-Replay-Download/1.0",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    ~HttpSession() { Close(); }
    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;

    bool Valid() const { return session_ != NULL; }

    HttpResult Get(const std::string& url,
                   const std::string& extra_headers,
                   const SegmentOptions& opts,
                   const SegmentCancel& cancel,
                   size_t max_bytes,
                   const std::string& stream_path)
    {
        HttpResult r;
        UrlParts u;
        if (!session_ || !ParseUrl(url, u)) return r;

        std::wstring whost = Utf8ToWide(u.host);
        if (connect_ && (host_ != whost || port_ != u.port || https_ != u.https)) {
            WinHttpCloseHandle(connect_);
            connect_ = NULL;
        }
        if (!connect_) {
            connect_ = WinHttpConnect(session_, whost.c_str(), (INTERNET_PORT)u.port, 0);
            if (!connect_) return r;
            host_ = whost;
            port_ = u.port;
            https_ = u.https;
        }

        std::wstring wobj = Utf8ToWide(u.path_query);
        DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(connect_, L"GET", wobj.c_str(), NULL,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) return r;

        WinHttpSetTimeouts(req, opts.connect_timeout_ms, opts.connect_timeout_ms,
                           opts.receive_timeout_ms, opts.receive_timeout_ms);

        std::string h;
        h += "User-Agent: " + opts.user_agent + "\r\n";
        h += "Accept: */*\r\n";
        h += "Accept-Encoding: identity\r\n";
        if (!opts.referer.empty()) h += "Referer: " + opts.referer + "\r\n";
        if (!opts.cookie.empty()) h += "Cookie: " + opts.cookie + "\r\n";
        if (!extra_headers.empty()) h += extra_headers;
        std::wstring wh = Utf8ToWide(h);
        WinHttpAddRequestHeaders(req, wh.c_str(), (DWORD)wh.size(),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            WinHttpCloseHandle(req);
            return r;
        }
        if (!WinHttpReceiveResponse(req, NULL)) {
            WinHttpCloseHandle(req);
            return r;
        }

        DWORD status = 0, status_size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX);
        r.status = (int)status;

        DWORD hsize = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, NULL, &hsize,
                            WINHTTP_NO_HEADER_INDEX);
        if (hsize > 0 && hsize < 1024u * 1024u) {
            size_t wchars = hsize / sizeof(wchar_t);
            std::vector<wchar_t> buf(wchars + 2, L'\0');
            DWORD got = hsize;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &buf[0], &got,
                                    WINHTTP_NO_HEADER_INDEX)) {
                size_t n = (size_t)got / sizeof(wchar_t);
                if (n > wchars) n = wchars;
                r.headers = WideToUtf8(std::wstring(&buf[0], n));
            }
        }
        r.network_ok = true;

        HANDLE out_file = INVALID_HANDLE_VALUE;
        if (!stream_path.empty()) {
            std::wstring wpath = Utf8ToWide(stream_path);
            out_file = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (out_file == INVALID_HANDLE_VALUE) {
                r.io_error = true;
                WinHttpCloseHandle(req);
                return r;
            }
        }

        char chunk[16384];
        size_t total = 0;
        bool clean_eof = false;
        while (total < max_bytes) {
            if (cancel()) break;
            DWORD want = (DWORD)std::min<size_t>(sizeof(chunk), max_bytes - total);
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk, want, &got)) break;
            if (got == 0) { clean_eof = true; break; }
            if (out_file != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                if (!WriteFile(out_file, chunk, got, &wrote, NULL) || wrote != got) {
                    r.io_error = true;
                    break;
                }
            } else {
                r.body.append(chunk, got);
            }
            total += got;
        }
        r.bytes = total;
        // For part downloads the byte cap is set to the exact expected size
        // (+1) for Range chunks or a generous 256 MiB for HLS chunks, so a
        // normal part always reaches a clean EOF. Leaving the loop on a read
        // error, on cancel, or by hitting the cap means the part is
        // incomplete: flag it so the caller retries / falls back instead of
        // silently concatenating a truncated segment.
        if (out_file != INVALID_HANDLE_VALUE && !clean_eof && !r.io_error)
            r.truncated = true;
        if (out_file != INVALID_HANDLE_VALUE) CloseHandle(out_file);
        WinHttpCloseHandle(req);
        return r;
    }

private:
    void Close()
    {
        if (connect_) { WinHttpCloseHandle(connect_); connect_ = NULL; }
        if (session_) { WinHttpCloseHandle(session_); session_ = NULL; }
    }
    HINTERNET session_ = NULL;
    HINTERNET connect_ = NULL;
    std::wstring host_;
    int port_ = 0;
    bool https_ = false;
};

int64_t ParseTotalFromContentRange(const std::string& headers)
{
    std::string low = Lower(headers);
    size_t p = low.find("content-range:");
    if (p == std::string::npos) return -1;
    size_t e = low.find('\n', p);
    std::string line = headers.substr(p, (e == std::string::npos ? headers.size() : e) - p);
    size_t slash = line.rfind('/');
    if (slash == std::string::npos) return -1;
    std::string num = Trim(line.substr(slash + 1));
    int64_t v = 0;
    bool any = false;
    for (size_t i = 0; i < num.size(); ++i) {
        char c = num[i];
        if (isdigit((unsigned char)c)) { v = v * 10 + (c - '0'); any = true; }
        else break;
    }
    return any ? v : -1;
}

// Byte offset a 206 response actually starts at ("Content-Range: bytes S-E/T").
int64_t ParseRangeStart(const std::string& headers)
{
    std::string low = Lower(headers);
    size_t p = low.find("content-range:");
    if (p == std::string::npos) return -1;
    size_t e = low.find('\n', p);
    std::string line = headers.substr(p, (e == std::string::npos ? headers.size() : e) - p);
    size_t b = low.find("bytes", p);
    if (b == std::string::npos) return -1;
    if (e != std::string::npos && b >= e) return -1;
    size_t lb = b - p;
    size_t dash = line.find('-', lb + 5);
    if (dash == std::string::npos) return -1;
    std::string num = Trim(line.substr(lb + 5, dash - (lb + 5)));
    int64_t v = 0;
    bool any = false;
    for (size_t i = 0; i < num.size(); ++i) {
        char c = num[i];
        if (isdigit((unsigned char)c)) { v = v * 10 + (c - '0'); any = true; }
        else break;
    }
    return any ? v : -1;
}

// ---------------------------------------------------------------------------
// AES-128 (HLS). libavutil is already linked statically via libffmpeg.lib.
// ---------------------------------------------------------------------------

std::vector<unsigned char> MakeIv(const std::string& iv_attr, uint64_t seq)
{
    std::vector<unsigned char> iv(16, 0);
    std::string s = iv_attr;
    if (StartsWith(Lower(s), "0x")) s = s.substr(2);
    if (!s.empty() && s.size() <= 32) {
        bool valid = true;
        for (size_t i = 0; i < s.size(); ++i)
            if (!isxdigit((unsigned char)s[i])) { valid = false; break; }
        if (valid) {
            std::string padded(32 - s.size(), '0');
            padded += s;
            for (size_t i = 0; i < 16; ++i) {
                char b[3];
                b[0] = padded[i * 2];
                b[1] = padded[i * 2 + 1];
                b[2] = '\0';
                iv[i] = (unsigned char)strtol(b, NULL, 16);
            }
            return iv;
        }
    }
    // HLS spec: without an IV attribute, use the media sequence number as a
    // 128-bit big-endian value.
    for (int i = 0; i < 8; ++i)
        iv[15 - i] = (unsigned char)((seq >> (8 * i)) & 0xff);
    return iv;
}

bool DecryptSegment(std::string& data, const std::string& key,
                    const std::vector<unsigned char>& iv)
{
    if (key.size() != 16 || iv.size() != 16) return false;
    if (data.empty() || (data.size() % 16) != 0) return false;
    struct AVAES* aes = av_aes_alloc();
    if (!aes) return false;
    if (av_aes_init(aes, (const uint8_t*)key.data(), 128, 1) < 0) {
        av_freep(&aes);
        return false;
    }
    std::vector<unsigned char> iv_copy(iv.begin(), iv.end());
    std::string out(data.size(), '\0');
    av_aes_crypt(aes, (uint8_t*)&out[0], (const uint8_t*)data.data(),
                 (int)(data.size() / 16), iv_copy.data(), 1);
    av_freep(&aes);

    // PKCS7 unpad, tolerant: some encoders omit padding, in which case we keep
    // the full plaintext (a TS demuxer ignores trailing garbage anyway).
    unsigned char pad = (unsigned char)out[out.size() - 1];
    if (pad >= 1 && pad <= 16 && (size_t)pad <= out.size()) {
        bool ok = true;
        for (size_t i = out.size() - pad; i < out.size(); ++i)
            if ((unsigned char)out[i] != pad) { ok = false; break; }
        if (ok) out.resize(out.size() - pad);
    }
    data.swap(out);
    return true;
}

void SleepWithCancel(int ms, const SegmentCancel& cancel)
{
    for (int elapsed = 0; elapsed < ms; elapsed += 100) {
        if (cancel()) return;
        Sleep((DWORD)std::min(100, ms - elapsed));
    }
}

// ---------------------------------------------------------------------------
// Ordered job runner (the C++ counterpart of Motrix-i's SegmentDownloader).
//
//   * a flat, ordered job list: [seg0, seg1, ...] or [range0, range1, ...]
//   * a fixed pool of worker threads = the concurrency semaphore
//   * per-job re-queue with capped retries (MAX_SELF_RETRIES)
//   * byte-accurate progress where the total size is known (Range mode)
//   * full cancel drain: workers join before the temp directory is removed
// ---------------------------------------------------------------------------

struct Job {
    size_t index = 0;
    std::string url;
    std::string out_path;
    std::string range_header;   // "Range: bytes=...\r\n" or empty
    std::string aes_key;        // 16 raw bytes; empty = no decryption
    std::vector<unsigned char> aes_iv;
    size_t max_bytes = kMaxSegmentBytes;
    size_t expected_bytes = 0;   // exact length required for Range parts
    int64_t range_start = -1;    // expected byte offset for a Range part
    int attempts_left = 0;
};

struct JobRunnerResult {
    bool ok = false;
    std::string error;          // ASCII
    size_t completed = 0;
    uint64_t bytes_done = 0;
};

class JobRunner {
public:
    JobRunner(std::vector<Job> jobs, const SegmentOptions& opts,
              const SegmentCancel& cancel, SegmentProgress progress,
              uint64_t bytes_total, SegmentLog log)
        : jobs_(std::move(jobs)), opts_(opts), cancel_(cancel),
          progress_(progress), log_(log), bytes_total_(bytes_total) {}

    JobRunnerResult Run()
    {
        JobRunnerResult result;
        total_ = jobs_.size();
        if (total_ == 0) { result.ok = true; progress_(1.0); return result; }

        for (size_t i = 0; i < jobs_.size(); ++i) pending_.push_back(i);

        size_t n = (size_t)std::max(1, std::min<int>(opts_.threads, (int)total_));
        std::vector<std::thread> workers;
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i)
            workers.emplace_back(&JobRunner::Worker, this, i);

        // Progress ticker: for Range downloads we know the exact byte total,
        // so report real byte progress; otherwise report completed parts.
        std::thread ticker(&JobRunner::ProgressLoop, this);

        for (size_t i = 0; i < workers.size(); ++i)
            if (workers[i].joinable()) workers[i].join();
        stop_.store(true);
        cv_.notify_all();
        if (ticker.joinable()) ticker.join();

        result.completed = completed_.load();
        result.bytes_done = bytes_done_.load();
        if (cancel_()) {
            result.ok = false;
            result.error = "cancelled";
            return result;
        }
        if (failed_.load()) {
            result.ok = false;
            result.error = first_error_.empty() ? "part download failed" : first_error_;
            return result;
        }
        if (result.completed != total_) {
            result.ok = false;
            result.error = "incomplete part set";
            return result;
        }
        result.ok = true;
        progress_(1.0);
        return result;
    }

private:
    void SetError(const std::string& e)
    {
        std::lock_guard<std::mutex> gk(err_mtx_);
        if (first_error_.empty()) first_error_ = e;
    }

    void ProgressLoop()
    {
        while (!stop_.load()) {
            double f;
            if (bytes_total_ > 0) {
                f = (double)(std::min<uint64_t>(bytes_done_.load(), bytes_total_)) /
                    (double)bytes_total_;
            } else {
                f = (double)completed_.load() / (double)total_;
            }
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            progress_(f);
            std::unique_lock<std::mutex> lk(cv_mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(kProgressIntervalMs),
                         [this] { return stop_.load(); });
        }
    }

    bool FetchOne(HttpSession& session, const Job& job, std::string& err)
    {
        HttpResult r = session.Get(job.url, job.range_header, opts_, cancel_,
                                   job.max_bytes, job.out_path);
        if (!r.network_ok) { err = "network request failed"; return false; }
        if (cancel_()) { err = "cancelled"; return false; }
        if (r.io_error) { err = "disk write failed"; return false; }
        if (r.truncated) { err = "incomplete part (connection ended early)"; return false; }
        if (r.status != 200 && r.status != 206) {
            err = "HTTP status " + std::to_string(r.status);
            return false;
        }
        if (!job.range_header.empty() && r.status != 206) {
            // Server ignored Range: downloading the whole body into one part
            // would corrupt the merge. Bail out and let the caller fall back.
            err = "server does not honour HTTP Range";
            return false;
        }
        if (job.range_start >= 0) {
            int64_t actual = ParseRangeStart(r.headers);
            if (actual >= 0 && actual != job.range_start) {
                err = "server returned an unexpected Range";
                return false;
            }
        }
        if (job.expected_bytes > 0 && r.bytes != job.expected_bytes) {
            err = "short or oversized Range response";
            return false;
        }
        if (r.bytes > 0) bytes_done_.fetch_add((uint64_t)r.bytes);

        if (!job.aes_key.empty()) {
            std::string data;
            if (!ReadBinary(job.out_path, data)) { err = "cannot re-read encrypted part"; return false; }
            if (!DecryptSegment(data, job.aes_key, job.aes_iv)) {
                err = "AES-128 decryption failed";
                return false;
            }
            if (!WriteBinary(job.out_path, data)) { err = "cannot write decrypted part"; return false; }
        }
        return true;
    }

    void Worker(size_t /*worker_index*/)
    {
        HttpSession session;
        if (!session.Valid()) {
            SetError("WinHttpOpen failed");
            failed_.store(true);
            return;
        }
        while (true) {
            if (cancel_() || failed_.load()) return;
            size_t idx;
            {
                std::lock_guard<std::mutex> gk(queue_mtx_);
                if (pending_.empty()) return;
                idx = pending_.front();
                pending_.pop_front();
            }

            Job& job = jobs_[idx];
            bool ok = false;
            std::string err;
            for (int attempt = 0; attempt <= opts_.tries; ++attempt) {
                if (cancel_() || failed_.load()) return;
                ok = FetchOne(session, job, err);
                if (ok) break;
                if (attempt < opts_.tries) {
                    std::wstring w = Utf8ToWide(job.out_path);
                    DeleteFileW(w.c_str());
                    SleepWithCancel(opts_.retry_wait_ms, cancel_);
                }
            }

            if (ok) {
                completed_.fetch_add(1);
                continue;
            }
            if (cancel_()) return;

            // Motrix-i's "self retry": re-queue the same job after its failed
            // result has been purged, keeping the semaphore count stable.
            if (job.attempts_left > 0) {
                --job.attempts_left;
                SleepWithCancel(opts_.retry_wait_ms, cancel_);
                {
                    std::lock_guard<std::mutex> gk(queue_mtx_);
                    pending_.push_back(idx);
                }
                continue;
            }
            SetError(err.empty() ? "part download failed" : err);
            failed_.store(true);
            cv_.notify_all();
            return;
        }
    }

    std::vector<Job> jobs_;
    const SegmentOptions& opts_;
    SegmentCancel cancel_;
    SegmentProgress progress_;
    SegmentLog log_;
    uint64_t bytes_total_ = 0;
    size_t total_ = 0;

    std::deque<size_t> pending_;
    std::mutex queue_mtx_;
    std::mutex cv_mtx_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> failed_{ false };
    std::atomic<size_t> completed_{ 0 };
    std::atomic<uint64_t> bytes_done_{ 0 };
    std::mutex err_mtx_;
    std::string first_error_;
};

// ---------------------------------------------------------------------------
// m3u8 parsing
// ---------------------------------------------------------------------------

struct PlaylistInfo {
    bool is_m3u8 = false;
    bool master = false;
    bool live = false;
    bool unsupported = false;      // fMP4 init / SAMPLE-AES / etc.
    std::string reason;
    uint64_t media_sequence = 0;
    struct Segment {
        std::string url;
        std::string range_header;   // from #EXT-X-BYTERANGE
        size_t byterange_len = 0;  // exact length of that Range
        int64_t range_start = -1;  // byte offset of that Range
        std::string key_uri;        // resolved absolute URL of the AES key
        std::vector<unsigned char> iv;
    };
    std::vector<Segment> segments;
    std::vector<std::string> lines; // original lines, for local rewrite
    std::vector<size_t> line_of_segment; // line index in lines[] for each segment
};

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t e = text.find('\n', start);
        if (e == std::string::npos) { out.push_back(text.substr(start)); break; }
        out.push_back(text.substr(start, e - start));
        start = e + 1;
    }
    return out;
}

std::string AttrValue(const std::string& line, const std::string& key)
{
    const std::string up = Upper(key) + "=";
    if (up.size() > line.size()) return std::string();
    for (size_t i = 0; i + up.size() <= line.size(); ++i) {
        bool eq = true;
        for (size_t k = 0; k < up.size(); ++k)
            if (toupper((unsigned char)line[i + k]) != up[k]) { eq = false; break; }
        if (!eq) continue;
        // Only match at an attribute-list boundary: a quoted URI may contain
        // e.g. "?iv=..." and must not be mistaken for the IV attribute.
        if (i > 0 && line[i - 1] != ',' && line[i - 1] != ':' &&
            !isspace((unsigned char)line[i - 1])) continue;
        size_t vs = i + up.size();
        if (vs >= line.size()) return std::string();
        if (line[vs] == '"') {
            size_t ve = line.find('"', vs + 1);
            if (ve == std::string::npos) return std::string();
            return line.substr(vs + 1, ve - vs - 1);
        }
        size_t ve = line.find(',', vs);
        if (ve == std::string::npos) ve = line.size();
        return Trim(line.substr(vs, ve - vs));
    }
    return std::string();
}

PlaylistInfo ParsePlaylist(const std::string& text, const std::string& base_url)
{
    PlaylistInfo pi;
    // UTF-8 BOM
    size_t off = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) off = 3;
    pi.lines = SplitLines(text.substr(off));

    std::string current_key_uri;
    std::string current_iv_attr;
    std::string pending_range;
    size_t pending_range_len = 0;
    int64_t pending_range_start = -1;
    uint64_t next_range_start = 0;
    bool saw_endlist = false;

    for (size_t li = 0; li < pi.lines.size(); ++li) {
        std::string line = Trim(pi.lines[li]);
        if (line.empty()) continue;
        if (line[0] == '#') {
            std::string up = Upper(line);
            if (StartsWith(up, "#EXTM3U")) pi.is_m3u8 = true;
            else if (StartsWith(up, "#EXT-X-STREAM-INF") ||
                     StartsWith(up, "#EXT-X-I-FRAME-STREAM-INF") ||
                     StartsWith(up, "#EXT-X-MEDIA:") ||
                     StartsWith(up, "#EXT-X-SESSION-KEY")) pi.master = true;
            else if (StartsWith(up, "#EXT-X-MAP:")) {
                // fMP4 init segments are handled by ffmpeg's own HLS demuxer;
                // fall back rather than re-implement init segment handling.
                pi.unsupported = true;
                pi.reason = "fMP4 playlist uses EXT-X-MAP";
            }
            else if (StartsWith(up, "#EXT-X-KEY:")) {
                std::string method = Upper(AttrValue(line, "METHOD"));
                if (method != "AES-128" && method != "NONE") {
                    pi.unsupported = true;
                    pi.reason = "unsupported encryption method";
                } else if (method == "NONE") {
                    current_key_uri.clear();
                    current_iv_attr.clear();
                } else {
                    std::string uri = AttrValue(line, "URI");
                    if (StartsWith(Lower(uri), "data:") || StartsWith(Lower(uri), "skd://")) {
                        pi.unsupported = true;
                        pi.reason = "unsupported key URI scheme";
                    } else {
                        current_key_uri = ResolveUrl(base_url, uri);
                        current_iv_attr = AttrValue(line, "IV");
                    }
                }
            }
            else if (StartsWith(up, "#EXT-X-BYTERANGE:")) {
                std::string spec = Trim(line.substr(17));
                size_t at = spec.find('@');
                std::string len_s = Trim(spec.substr(0, at));
                std::string off_s = (at == std::string::npos) ? std::string() : Trim(spec.substr(at + 1));
                uint64_t len = ParseUInt(len_s);
                // HLS spec: when the offset is omitted the sub-range starts
                // right after the previous BYTERANGE segment's sub-range.
                uint64_t start = (at == std::string::npos) ? next_range_start : ParseUInt(off_s);
                if (len > 0) {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "Range: bytes=%llu-%llu\r\n",
                             (unsigned long long)start, (unsigned long long)(start + len - 1));
                    pending_range = buf;
                    pending_range_len = (size_t)len;
                    pending_range_start = (int64_t)start;
                    next_range_start = start + len;
                }
            }
            else if (StartsWith(up, "#EXT-X-MEDIA-SEQUENCE:")) {
                pi.media_sequence = ParseUInt(Trim(line.substr(22)));
            }
            else if (StartsWith(up, "#EXT-X-ENDLIST")) saw_endlist = true;
            continue;
        }
        // media segment line
        // Only bare MPEG-TS parts can be safely concatenated. Independent
        // fMP4 / MP4 / elementary-audio parts need an init segment, so leave
        // them to ffmpeg's own HLS demuxer (strict fallback).
        std::string seg_low = Lower(line);
        size_t qpos = seg_low.find('?');
        if (qpos != std::string::npos) seg_low.resize(qpos);
        std::string ext;
        size_t dot = seg_low.rfind('.');
        size_t slash = seg_low.rfind('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            ext = seg_low.substr(dot);
        if (ext == ".m4s" || ext == ".mp4" || ext == ".fmp4" ||
            ext == ".aac" || ext == ".mp3" || ext == ".webm" ||
            ext == ".m4a" || ext == ".vtt") {
            pi.unsupported = true;
            pi.reason = "non-TS segment container requires ffmpeg demuxer";
        }
        PlaylistInfo::Segment seg;
        seg.url = ResolveUrl(base_url, line);
        seg.range_header = pending_range;
        seg.byterange_len = pending_range_len;
        seg.range_start = pending_range_start;
        pending_range.clear();
        pending_range_len = 0;
        pending_range_start = -1;
        if (!current_key_uri.empty()) {
            seg.key_uri = current_key_uri;
            seg.iv = MakeIv(current_iv_attr, pi.media_sequence + pi.segments.size());
        }
        pi.line_of_segment.push_back(li);
        pi.segments.push_back(seg);
    }
    if (!saw_endlist) pi.live = true;
    return pi;
}

// ---------------------------------------------------------------------------
// Temp directory
// ---------------------------------------------------------------------------

std::string MakeTempDir()
{
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return std::string();
    wchar_t full[MAX_PATH];
    if (!GetTempFileNameW(tmp, L"crd", 0, full)) return std::string();
    DeleteFileW(full);
    if (!CreateDirectoryW(full, NULL)) return std::string();
    return PadDir(WideToUtf8(std::wstring(full)));
}

void RemoveTree(const std::string& dir)
{
    if (dir.empty()) return;
    std::wstring wdir = Utf8ToWide(dir);
    size_t len = wdir.size();
    while (len > 0 && (wdir[len - 1] == L'\\' || wdir[len - 1] == L'/')) --len;
    std::wstring pattern = wdir.substr(0, len) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = wdir.substr(0, len) + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(WideToUtf8(child));
            else
                DeleteFileW(child.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(wdir.substr(0, len).c_str());
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

SegmentResult DownloadSegmentsImpl(const std::string& url,
                               const SegmentOptions& options,
                               const SegmentCancel& is_cancelled,
                               SegmentProgress on_progress,
                               SegmentLog on_log)
{
    SegmentResult result;
    const SegmentCancel& cancel = is_cancelled;
    if (!on_progress) on_progress = [](double) {};
    if (!on_log) on_log = [](const std::string&) {};

    UrlParts u;
    if (!ParseUrl(url, u)) {
        result.error = "unsupported URL scheme (http/https only)";
        on_log("segment: " + result.error + "; falling back to single-thread remux");
        return result;
    }

    HttpSession probe;
    if (!probe.Valid()) {
        result.error = "WinHttpOpen failed";
        on_log("segment: " + result.error + "; falling back to single-thread remux");
        return result;
    }

    // Step 1: probe only the first 64 KiB. A plain MP4 must never be fetched
    // in full here; a Range request keeps it bounded even if the server
    // ignores Range (we simply close the handle after 64 KiB).
    HttpResult head = probe.Get(url, "Range: bytes=0-65535\r\n", options, cancel,
                                kProbeBytes, std::string());
    if (cancel()) { result.cancelled = true; result.error = "cancelled"; return result; }
    if (!head.network_ok || (head.status != 200 && head.status != 206)) {
        result.error = "HEAD probe failed (HTTP " + std::to_string(head.status) + ")";
        on_log("segment: " + result.error + "; falling back to single-thread remux");
        return result;
    }

    std::string prefix = head.body;
    size_t bom = 0;
    if (prefix.size() >= 3 && (unsigned char)prefix[0] == 0xEF &&
        (unsigned char)prefix[1] == 0xBB && (unsigned char)prefix[2] == 0xBF) bom = 3;
    std::string trimmed = Trim(prefix.substr(bom));
    size_t first_nl = trimmed.find('\n');
    std::string first_line = Trim(trimmed.substr(0, first_nl));
    bool looks_playlist = StartsWith(Upper(first_line), "#EXTM3U");

    // -----------------------------------------------------------------------
    // Path A: HLS media playlist -> concurrent .ts / .m4s part downloads.
    // This is the ClassIn replay case (playback.eeo.cn *.m3u8).
    // -----------------------------------------------------------------------
    if (looks_playlist) {
        // Only reuse the probe body when the server returned a plain 200 that
        // was clearly complete. Otherwise (206 Range, or a truncated body) do
        // one full GET so the playlist is never parsed half-way.
        std::string playlist;
        if (head.status == 200 && prefix.size() < kProbeBytes) {
            playlist = prefix;
        } else {
            HttpResult full = probe.Get(url, std::string(), options, cancel,
                                        kMaxPlaylistBytes, std::string());
            if (!full.network_ok || full.status != 200 || full.body.empty()) {
                result.error = "cannot fetch full playlist";
                on_log("segment: " + result.error + "; falling back to single-thread remux");
                return result;
            }
            playlist = full.body;
        }
        if (cancel()) { result.cancelled = true; result.error = "cancelled"; return result; }

        PlaylistInfo pi = ParsePlaylist(playlist, url);
        if (!pi.is_m3u8 || pi.master) {
            result.error = pi.master ? "master playlist (variant selection belongs to ffmpeg)"
                                     : "not an HLS playlist";
            on_log("segment: " + result.error + "; falling back to single-thread remux");
            return result;
        }
        if (pi.live) {
            result.error = "live playlist has no ENDLIST";
            on_log("segment: " + result.error + "; falling back to single-thread remux");
            return result;
        }
        if (pi.unsupported) {
            result.error = pi.reason;
            on_log("segment: " + result.error + "; falling back to single-thread remux");
            return result;
        }
        if (pi.segments.empty() || pi.segments.size() > 20000) {
            result.error = "playlist has no usable segments";
            on_log("segment: " + result.error + "; falling back to single-thread remux");
            return result;
        }

        std::string temp_dir = MakeTempDir();
        if (temp_dir.empty()) {
            result.error = "cannot create temp directory";
            on_log("segment: " + result.error + "; falling back to single-thread remux");
            return result;
        }

        // AES-128 keys are tiny; fetch each distinct one once with the probe
        // session before starting the worker pool.
        std::map<std::string, std::string> key_cache;
        for (size_t i = 0; i < pi.segments.size(); ++i) {
            const std::string& ku = pi.segments[i].key_uri;
            if (ku.empty() || key_cache.count(ku)) continue;
            HttpResult k = probe.Get(ku, std::string(), options, cancel, 4096, std::string());
            if (cancel()) { result.cancelled = true; RemoveTree(temp_dir); return result; }
            if (!k.network_ok || k.status != 200 || k.body.size() != 16) {
                result.error = "cannot fetch AES-128 key";
                on_log("segment: " + result.error + "; falling back to single-thread remux");
                RemoveTree(temp_dir);
                return result;
            }
            key_cache[ku] = k.body;
        }

        std::vector<Job> jobs;
        jobs.reserve(pi.segments.size());
        for (size_t i = 0; i < pi.segments.size(); ++i) {
            Job job;
            job.index = i;
            job.url = pi.segments[i].url;
            job.out_path = temp_dir + PartName(i);
            job.range_header = pi.segments[i].range_header;
            job.aes_key = pi.segments[i].key_uri.empty() ? std::string() : key_cache[pi.segments[i].key_uri];
            job.aes_iv = pi.segments[i].iv;
            job.range_start = pi.segments[i].range_start;
            job.max_bytes = kMaxSegmentBytes;
            if (pi.segments[i].byterange_len > 0) {
                job.expected_bytes = pi.segments[i].byterange_len;
                job.max_bytes = pi.segments[i].byterange_len + 1;
            }
            job.attempts_left = options.self_retries;
            jobs.push_back(std::move(job));
        }

        on_log("segment: HLS playlist with " + std::to_string(jobs.size()) +
               " parts, " + std::to_string(std::max(1, std::min<int>(options.threads, (int)jobs.size()))) +
               " concurrent workers");

        JobRunner runner(std::move(jobs), options, cancel, on_progress, 0, on_log);
        JobRunnerResult r = runner.Run();
        if (cancel()) { result.cancelled = true; result.error = "cancelled"; RemoveTree(temp_dir); return result; }
        if (!r.ok) {
            result.error = r.error;
            on_log("segment: concurrent HLS failed (" + r.error + "); falling back to single-thread remux");
            RemoveTree(temp_dir);
            return result;
        }

        // MPEG-TS segments are concatenable by design. Merge them strictly in
        // playlist order (they were fetched out of order) into one .ts file,
        // which is a plain local path ffmpeg can open without URI resolution.
        std::string merged = temp_dir + "merged.ts";
        std::wstring wmerged = Utf8ToWide(merged);
        DeleteFileW(wmerged.c_str());
        bool merge_ok = true;
        for (size_t i = 0; i < r.completed; ++i)
            if (!AppendFile(merged, temp_dir + PartName(i))) { merge_ok = false; break; }
        if (!merge_ok) {
            result.error = "cannot concatenate HLS parts";
            on_log("segment: " + result.error + "; falling back to single-thread remux");
            RemoveTree(temp_dir);
            return result;
        }

        result.used_parallel = true;
        result.local_input = merged;
        result.temp_dir = temp_dir;
        result.parts = (int)r.completed;
        result.threads = std::max(1, std::min<int>(options.threads, result.parts));
        result.progress = 1.0;
        on_log("segment: all " + std::to_string(result.parts) + " parts downloaded in parallel");
        return result;
    }

    // -----------------------------------------------------------------------
    // Path B: a single media file -> aria2-style split / Range concurrency.
    // Only worth it when the server honours Range, the file is large enough,
    // and more than one connection is allowed. Otherwise ffmpeg can stream it
    // just as well on its own.
    // -----------------------------------------------------------------------
    int64_t total = ParseTotalFromContentRange(head.headers);
    if (head.status != 206 || total <= 0) {
        result.error = "server does not advertise HTTP Range";
        on_log("segment: " + result.error + "; falling back to single-thread remux");
        return result;
    }
    if (total < options.range_min_split_size || options.range_splits <= 1 || options.threads <= 1) {
        result.error = "file too small or parallel Range disabled";
        on_log("segment: " + result.error + "; falling back to single-thread remux");
        return result;
    }

    int splits = std::max(2, std::min<int>(options.range_splits, options.threads));
    std::string temp_dir = MakeTempDir();
    if (temp_dir.empty()) {
        result.error = "cannot create temp directory";
        on_log("segment: " + result.error + "; falling back to single-thread remux");
        return result;
    }

    int64_t chunk = total / splits;
    std::vector<Job> jobs;
    jobs.reserve((size_t)splits);
    for (int i = 0; i < splits; ++i) {
        int64_t start = chunk * (int64_t)i;
        int64_t end = (i == splits - 1) ? (total - 1) : (start + chunk - 1);
        Job job;
        job.index = (size_t)i;
        job.url = url;
        job.out_path = temp_dir + PartName((size_t)i);
        char buf[96];
        snprintf(buf, sizeof(buf), "Range: bytes=%lld-%lld\r\n",
                 (long long)start, (long long)end);
        job.range_header = buf;
        job.expected_bytes = (size_t)(end - start + 1);
        job.max_bytes = job.expected_bytes + 1;
        job.range_start = start;
        job.attempts_left = options.self_retries;
        jobs.push_back(std::move(job));
    }

    on_log("segment: single file " + std::to_string(total) + " bytes, " +
           std::to_string(splits) + " concurrent Range parts");

    JobRunner runner(std::move(jobs), options, cancel, on_progress,
                     (uint64_t)total, on_log);
    JobRunnerResult r = runner.Run();
    if (cancel()) { result.cancelled = true; result.error = "cancelled"; RemoveTree(temp_dir); return result; }
    if (!r.ok) {
        result.error = r.error;
        on_log("segment: concurrent Range failed (" + r.error + "); falling back to single-thread remux");
        RemoveTree(temp_dir);
        return result;
    }

    // Reassemble strictly in order (the parts were fetched out of order).
    std::string merged = temp_dir + "merged" + GuessExtension(url);
    std::wstring wmerged = Utf8ToWide(merged);
    DeleteFileW(wmerged.c_str());
    bool merge_ok = true;
    for (int i = 0; i < splits; ++i)
        if (!AppendFile(merged, temp_dir + PartName((size_t)i))) { merge_ok = false; break; }
    if (!merge_ok) {
        result.error = "cannot reassemble Range parts";
        on_log("segment: " + result.error + "; falling back to single-thread remux");
        RemoveTree(temp_dir);
        return result;
    }

    result.used_parallel = true;
    result.local_input = merged;
    result.temp_dir = temp_dir;
    result.parts = splits;
    result.threads = splits;
    result.progress = 1.0;
    on_log("segment: " + std::to_string(splits) + " Range parts merged");
    return result;
}

} // namespace

// Public symbol declared in segment_downloader.h. The implementation lives
// in the anonymous namespace above; an unqualified call resolves through the
// implicit using-directive of that namespace within this translation unit.
SegmentResult DownloadSegments(const std::string& url,
                               const SegmentOptions& options,
                               const SegmentCancel& is_cancelled,
                               SegmentProgress on_progress,
                               SegmentLog on_log)
{
    return DownloadSegmentsImpl(url, options, is_cancelled, on_progress, on_log);
}

// Best-effort recursive cleanup (never throws).
void CleanupTempDir(const std::string& dir)
{
    RemoveTree(dir);
}

