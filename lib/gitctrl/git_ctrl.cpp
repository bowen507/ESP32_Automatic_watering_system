#include "git_ctrl.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <Update.h>

/*
 * GitCtrl (Gitee)
 * ============
 *
 * 这个文件做三件事：
 * 1) OTA：下载固件 bin 并写入 OTA 分区，成功后重启
 * 2) 版本检测：从 version.json 判断是否有新版本，并输出固件下载 URL
 * 3) 远程小文件：downloadText() 用于下载 setting_online.json 这类小 JSON，做远程参数控制
 *
 * 为什么要分“固件大文件”和“配置小文件”：
 * - 大文件（~1MB）在 ESP32 + TLS 上更容易出现中途断流，成功率不稳定
 * - 小文件（<几 KB）成功率高，适合做“至少有一个稳定远程控制办法”
 */

static String normalizeUrl(String url) {
    // 清理 URL：去除首尾空白与换行/空格，避免串口打印或拼接导致 URL 被拆成两行
    url.trim();
    String out;
    out.reserve(url.length());
    for (size_t i = 0; i < (size_t)url.length(); i++) {
        char c = url.charAt((unsigned int)i);
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') continue;
        out += c;
    }
    return out;
}

static bool parseGiteeOwnerRepoFromUrl(const String& url, String& ownerOut, String& repoOut) {
    // 从 gitee.com/{owner}/{repo}/... 中解析 owner/repo（用于自动拼接 Releases 下载链接）
    ownerOut = "";
    repoOut = "";

    int idx = url.indexOf("gitee.com/");
    if (idx < 0) return false;

    int start = idx + (int)strlen("gitee.com/");
    int ownerEnd = url.indexOf('/', start);
    if (ownerEnd < 0) return false;

    int repoStart = ownerEnd + 1;
    int repoEnd = url.indexOf('/', repoStart);
    if (repoEnd < 0) repoEnd = url.length();

    ownerOut = url.substring(start, ownerEnd);
    repoOut = url.substring(repoStart, repoEnd);
    return ownerOut.length() > 0 && repoOut.length() > 0;
}

static String buildGiteeReleaseFirmwareUrl(const String& owner, const String& repo, const String& version) {
    // 拼接 Gitee Releases 固件下载地址：
    // https://gitee.com/{owner}/{repo}/releases/download/v{version}/firmware.bin
    if (owner.isEmpty() || repo.isEmpty() || version.isEmpty()) return "";
    String tag = version;
    if (!(tag.startsWith("v") || tag.startsWith("V"))) {
        tag = "v" + tag;
    }
    return "https://gitee.com/" + owner + "/" + repo + "/releases/download/" + tag + "/firmware.bin";
}

static String stripQueryParam(const String& url, const String& key) {
    // 删除 URL query 中的某个参数（例如 access_token），用于把 URL 规范化
    int q = url.indexOf('?');
    if (q < 0) return url;

    String base = url.substring(0, q);
    String query = url.substring(q + 1);
    String out;

    int start = 0;
    while (start < query.length()) {
        int amp = query.indexOf('&', start);
        if (amp < 0) amp = query.length();
        String part = query.substring(start, amp);
        if (part.length() > 0 && !part.startsWith(key + "=")) {
            if (out.length() > 0) out += "&";
            out += part;
        }
        start = amp + 1;
    }

    if (out.length() == 0) return base;
    return base + "?" + out;
}

static String appendQueryParam(const String& url, const String& key, const String& value) {
    // 追加 query 参数（当前工程里对 Releases 下载默认不使用 token）
    if (value.isEmpty()) return url;
    if (url.indexOf(key + "=") != -1) return url;
    return url + (url.indexOf('?') == -1 ? "?" : "&") + key + "=" + value;
}

static bool parseHttpUrl(const String& url, bool& isHttps, String& host, uint16_t& port, String& uri) {
    // 解析 http(s)://host[:port]/path，用于打印调试信息（DNS/端口等）
    isHttps = false;
    host = "";
    uri = "/";
    port = 80;

    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) return false;

    String scheme = url.substring(0, schemeEnd);
    scheme.toLowerCase();
    isHttps = (scheme == "https");
    port = isHttps ? 443 : 80;

    int hostStart = schemeEnd + 3;
    int pathStart = url.indexOf('/', hostStart);
    String hostPort = (pathStart < 0) ? url.substring(hostStart) : url.substring(hostStart, pathStart);
    uri = (pathStart < 0) ? "/" : url.substring(pathStart);

    int colon = hostPort.indexOf(':');
    if (colon >= 0) {
        host = hostPort.substring(0, colon);
        port = (uint16_t)hostPort.substring(colon + 1).toInt();
    } else {
        host = hostPort;
    }

    return host.length() > 0;
}

static String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < (size_t)in.length(); i++) {
        char c = in.charAt((unsigned int)i);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    // control chars -> \u00XX
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static const size_t OTA_SEGMENT_SIZE = 262144;  // 256KB/段，~15s TLS 连接，远低于 CDN 50s 超时

// 解析 Content-Range: bytes 0-262143/1229632 → 返回 total
static int parseContentRangeTotal(const String& header) {
    int slash = header.lastIndexOf('/');
    if (slash < 0) return -1;
    return header.substring(slash + 1).toInt();
}

// 解析 CDN 直连地址：GET Gitee URL 不跟随重定向，提取 Location 头
static String resolveCdnUrl(const String& giteeUrl) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);
#if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(10000);
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32WaterBot/1.0");
    http.addHeader("Accept", "*/*");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(8000);
    http.setTimeout(10000);

    unsigned long t0 = millis();
    if (!http.begin(client, giteeUrl)) {
        Serial.printf("[GitCtrl] CDN resolve: http.begin failed (%lums)\n", millis() - t0);
        client.stop(); return "";
    }
    const char* locationKeys[] = { "Location" };
    http.collectHeaders(locationKeys, 1);
    Serial.printf("[GitCtrl] CDN resolve: connected (%lums), sending GET...\n", millis() - t0);
    int code = http.GET();
    Serial.printf("[GitCtrl] CDN resolve: GET returned %d (%lums)\n", code, millis() - t0);
    String loc = (code == 301 || code == 302) ? http.header("Location") : "";
    http.end(); client.stop();
    return loc;
}

// 获取文件总大小（Range: bytes=0-0 → Content-Range header）
static int probeFileSize(const String& cdnUrl) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);
#if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(10000);
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32WaterBot/1.0");
    http.addHeader("Accept", "*/*");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    http.addHeader("Range", "bytes=0-0");

    if (!http.begin(client, cdnUrl)) {
        Serial.println("[GitCtrl] Size probe: http.begin failed");
        client.stop(); return -1;
    }
    const char* hk[] = { "Content-Range", "Content-Length" };
    http.collectHeaders(hk, 2);

    int code = http.GET();
    Serial.printf("[GitCtrl] Size probe HTTP %d\n", code);

    // CDN 可能返回 206（支持 Range）、200（不支持 Range）、或再次 302
    if (code == 206) {
        String cr = http.header("Content-Range");
        Serial.printf("[GitCtrl] Content-Range: %s\n", cr.c_str());
        int total = parseContentRangeTotal(cr);
        http.end(); client.stop();
        return total;
    }
    if (code == 200) {
        int total = http.getSize();
        Serial.printf("[GitCtrl] Content-Length (200): %d\n", total);
        http.end(); client.stop();
        return (total > 0) ? total : -1;
    }
    // 302 / 其他
    if (code == 301 || code == 302) {
        Serial.printf("[GitCtrl] Size probe: CDN redirected again (%d)\n", code);
    }
    http.end(); client.stop();
    return -1;
}

// 下载单个 Range 段（直连 CDN，无重定向，Range 头不会丢失）
static size_t downloadSegment(const String& cdnUrl, size_t rangeStart, size_t rangeEnd,
                              size_t& writtenTotal, size_t totalSize, int segIdx, int segCount) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30000);
#if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(10000);
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32WaterBot/1.0");
    http.addHeader("Accept", "*/*");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(10000);
    http.setTimeout(30000);

    String rangeHeader = "bytes=" + String(rangeStart) + "-" + String(rangeEnd);
    http.addHeader("Range", rangeHeader);

    if (!http.begin(client, cdnUrl)) {
        Serial.printf("[GitCtrl] S%d http.begin failed\n", segIdx);
        client.stop(); return 0;
    }

    int httpCode = http.GET();
    // 206 = Partial Content, 200 = OK (CDN 可能不区分 Range)
    if (httpCode != 200 && httpCode != 206) {
        Serial.printf("[GitCtrl] S%d HTTP %d\n", segIdx, httpCode);
        http.end(); client.stop(); return 0;
    }

    bool chunked = (http.header("Transfer-Encoding").indexOf("chunked") >= 0);
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) { http.end(); client.stop(); return 0; }
    stream->setTimeout(30000);

    uint8_t buff[1024];
    size_t segWritten = 0;
    size_t expectedSegSize = rangeEnd - rangeStart + 1;

    // 第一段校验固件魔数
    if (rangeStart == 0) {
        uint8_t firstByte = 0;
        if (chunked) {
            String hexStr; hexStr.reserve(8);
            while (true) {
                int c = stream->read();
                if (c < 0) { Serial.printf("[GitCtrl] S%d chunk hdr err\n", segIdx); http.end(); client.stop(); return 0; }
                if (c == '\r') { stream->read(); break; }
                hexStr += (char)c;
            }
        }
        if (stream->readBytes(&firstByte, 1) != 1) {
            Serial.printf("[GitCtrl] S%d failed to read first byte\n", segIdx);
            http.end(); client.stop(); return 0;
        }
        if (firstByte != 0xE9) {
            uint8_t sample[32] = {0}; sample[0] = firstByte;
            size_t extra = stream->readBytes(sample + 1, sizeof(sample) - 1);
            Serial.printf("[GitCtrl] S%d bad magic 0x%02X: ", segIdx, firstByte);
            for (size_t i = 0; i < 1 + extra; i++) Serial.printf("%02X ", sample[i]);
            Serial.println();
            http.end(); client.stop(); return 0;
        }
        Update.write(&firstByte, 1);
        segWritten = 1;
        writtenTotal = 1;
    }

    // 读取本段
    if (chunked) {
        size_t chunkDataLeft = 0;
        while (segWritten < expectedSegSize) {
            if (chunkDataLeft == 0) {
                int cr = stream->read(); if (cr == '\r') stream->read();
                String hexStr; hexStr.reserve(8);
                while (true) {
                    int c = stream->read();
                    if (c < 0) { Serial.printf("[GitCtrl] S%d chunk err\n", segIdx); http.end(); client.stop(); return segWritten; }
                    if (c == '\r') { stream->read(); break; }
                    hexStr += (char)c;
                }
                chunkDataLeft = strtoul(hexStr.c_str(), nullptr, 16);
                if (chunkDataLeft == 0) break;
            }
            size_t toRead = chunkDataLeft > sizeof(buff) ? sizeof(buff) : chunkDataLeft;
            if (toRead > (expectedSegSize - segWritten)) toRead = expectedSegSize - segWritten;
            int r = stream->readBytes(buff, toRead);
            if (r <= 0) { delay(10); continue; }
            Update.write(buff, (size_t)r);
            segWritten += r; chunkDataLeft -= r; writtenTotal += r;
        }
    } else {
        while (segWritten < expectedSegSize) {
            size_t toRead = expectedSegSize - segWritten;
            if (toRead > sizeof(buff)) toRead = sizeof(buff);
            int r = stream->readBytes(buff, toRead);
            if (r <= 0) { delay(10); continue; }
            Update.write(buff, (size_t)r);
            segWritten += r; writtenTotal += r;
        }
    }

    http.end();
    client.stop();
    return segWritten;
}

// ---- OTA 下载（分段 / 流式）----
static bool otaUpdateViaStream(const String& giteeUrl) {
    size_t freeHeap = ESP.getFreeHeap();
    Serial.printf("[GitCtrl] OTA free heap: %u bytes\n", freeHeap);
    if (freeHeap < 80 * 1024) {
        Serial.printf("[GitCtrl] OTA aborted: heap too low (%u < 81920)\n", freeHeap);
        return false;
    }

    // Step 1: 解析 CDN 直连地址
    Serial.println("[GitCtrl] Resolving CDN URL...");
    String cdnUrl = resolveCdnUrl(giteeUrl);
    if (cdnUrl.isEmpty()) {
        Serial.println("[GitCtrl] Failed to resolve CDN URL");
        return false;
    }
    Serial.printf("[GitCtrl] CDN: %s...\n", cdnUrl.substring(0, 50).c_str());

    // Step 2: 直连 CDN，GET 完整文件（HTTPClient 自动处理 redirect + chunked）
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30000);
#if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(15000);
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32WaterBot/1.0");
    http.addHeader("Accept", "*/*");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(15000);
    http.setTimeout(30000);  // 30s 读超时，uint16_t 最大 65535

    if (!http.begin(client, cdnUrl)) {
        Serial.println("[GitCtrl] CDN http.begin failed");
        client.stop(); return false;
    }
    const char* respKeys[] = { "Transfer-Encoding", "Content-Length" };
    http.collectHeaders(respKeys, 2);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[GitCtrl] CDN GET returned %d\n", code);
        http.end(); client.stop(); return false;
    }

    Serial.printf("[GitCtrl] Transfer-Encoding: %s\n", http.header("Transfer-Encoding").c_str());
    Serial.printf("[GitCtrl] Content-Length: %s\n", http.header("Content-Length").c_str());

    int total = http.getSize();
    if (total <= 0) {
        // CDN 用了 chunked 编码，不提供 Content-Length
        total = 0x1E0000;  // OTA 分区大小（partitions.csv）
        Serial.printf("[GitCtrl] No Content-Length, using partition size: %d\n", total);
    }
    Serial.printf("[GitCtrl] Firmware expect: %d bytes (%.1f MB)\n", total, total / 1048576.0f);

    // Step 3: 初始化 OTA 分区
    if (!Update.begin((size_t)total)) {
        Serial.printf("[GitCtrl] Update.begin failed, err=%u\n", Update.getError());
        http.end(); client.stop(); return false;
    }

    // Step 4: 逐块读取 → Update.write()，兼容 chunked 与非 chunked
    unsigned long t0 = millis();
    WiFiClient* stream = http.getStreamPtr();
    bool chunked = (http.header("Transfer-Encoding").indexOf("chunked") >= 0);
    uint8_t buff[1024];
    size_t written = 0;
    unsigned long lastData = millis();
    bool magicChecked = false;

    if (chunked) {
        Serial.println("[GitCtrl] Stream is chunked");
        size_t chunkLeft = 0;
        bool firstChunk = true;
        while (true) {
            if (chunkLeft == 0) {
                String hexStr; hexStr.reserve(8);
                while (true) {
                    int c = stream->read();
                    if (c < 0) goto done;
                    // 首个 chunk 前 CDN 可能多一个 \r\n 空行，跳过它
                    if (c == '\r' && firstChunk && hexStr.length() == 0) {
                        stream->read(); // skip \n
                        continue;
                    }
                    if (c == '\r') { stream->read(); break; }
                    hexStr += (char)c;
                }
                firstChunk = false;
                chunkLeft = strtoul(hexStr.c_str(), nullptr, 16);
                if (chunkLeft == 0) break;
                Serial.printf("[GitCtrl] Next chunk: %u bytes\n", (unsigned)chunkLeft);
            }
            size_t toRead = chunkLeft > sizeof(buff) ? sizeof(buff) : chunkLeft;
            int r = stream->readBytes(buff, toRead);
            if (r <= 0) break;
            if (!magicChecked) {
                magicChecked = true;
                Serial.printf("[GitCtrl] First byte: 0x%02X\n", buff[0]);
                if (buff[0] != 0xE9) {
                    Serial.printf("[GitCtrl] Bad magic! First 32 bytes: ");
                    for (int i = 0; i < 32 && i < r; i++) Serial.printf("%02X ", buff[i]);
                    Serial.println();
                    goto abort;
                }
            }
            Update.write(buff, (size_t)r);
            written += r;
            chunkLeft -= r;
            lastData = millis();
            if (chunkLeft == 0) { stream->read(); stream->read(); }
        }
    } else {
        while (stream->connected() || stream->available()) {
            int r = stream->read(buff, sizeof(buff));
            if (r > 0) {
                if (!magicChecked) {
                    magicChecked = true;
                    Serial.printf("[GitCtrl] First byte: 0x%02X\n", buff[0]);
                    if (buff[0] != 0xE9) {
                        Serial.printf("[GitCtrl] Bad magic! First 32 bytes: ");
                        for (int i = 0; i < 32 && i < r; i++) Serial.printf("%02X ", buff[i]);
                        Serial.println();
                        goto abort;
                    }
                }
                Update.write(buff, (size_t)r);
                written += r;
                lastData = millis();
            } else if (r < 0) {
                break;
            }
            if (millis() - lastData > 90000) break;
        }
    }
    goto done;
abort:
    http.end();
    client.stop();
    Update.abort();
    return false;
done:
    unsigned long elapsed = millis() - t0;

    http.end();
    client.stop();

    Serial.printf("[GitCtrl] Downloaded %u bytes in %lu ms (%.1f KB/s)\n",
                 (unsigned)written, elapsed, written / (float)elapsed * 1000.0f / 1024.0f);

    // Step 5: 校验 & 完成
    if (written < 64 * 1024) {
        Serial.printf("[GitCtrl] Too small: %u bytes\n", (unsigned)written);
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        Serial.printf("[GitCtrl] Update.end failed, err=%u\n", Update.getError());
        return false;
    }

    Serial.println("[GitCtrl] OTA Update OK");
    return true;
}

// COS 固件下载 — 使用 ESP32 官方 HTTPUpdate 库，稳定可靠
static bool otaUpdateDirect(const String& directUrl) {
    size_t freeHeap = ESP.getFreeHeap();
    Serial.printf("[GitCtrl] OTA(Direct) free heap: %u bytes\n", freeHeap);
    if (freeHeap < 80 * 1024) {
        Serial.printf("[GitCtrl] OTA(Direct) aborted: heap too low (%u < 81920)\n", freeHeap);
        return false;
    }

    WiFiClient client;
    client.setTimeout(180);  // 3 分钟超时，1.2MB 足够

    HTTPUpdate httpUpdate;
    httpUpdate.rebootOnUpdate(false);  // 我们自己控制重启

    // 进度回调：每 10% 打印一次
    httpUpdate.onProgress([](int cur, int total) {
        static int lastPct = -1;
        int pct = (total > 0) ? (cur * 100 / total) : 0;
        if (pct >= lastPct + 10 || pct == 100) {
            Serial.printf("[GitCtrl] OTA Progress: %d/%d (%d%%)\n", cur, total, pct);
            lastPct = pct;
        }
    });

    Serial.printf("[GitCtrl] OTA(Direct) Downloading: %s\n", directUrl.c_str());
    unsigned long t0 = millis();

    t_httpUpdate_return ret = httpUpdate.update(client, directUrl);

    unsigned long elapsed = millis() - t0;

    switch (ret) {
        case HTTP_UPDATE_OK:
            Serial.printf("[GitCtrl] OTA(Direct) OK (%lu ms)\n", elapsed);
            return true;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("[GitCtrl] OTA(Direct) No updates (server returned 304)");
            return false;
        case HTTP_UPDATE_FAILED:
            Serial.printf("[GitCtrl] OTA(Direct) Failed (%lu ms): %s\n",
                         elapsed, httpUpdate.getLastErrorString().c_str());
            return false;
    }
    return false;
}

GitCtrl::GitCtrl(String token, String owner, String repo) {
    _token = token;
    _owner = owner;
    _repo = repo;
}

bool GitCtrl::downloadText(String url, String &contentOut) {
    /*
     * 下载小文本/JSON（用于 setting_online.json）
     * -------------------------------------------
     * 设计目标：稳定、简单
     * - 文件很小：直接 http.getString() 读完
     * - 强制跟随重定向：Gitee 常见 302
     * - 禁用压缩：避免解析失败
     */
    contentOut = "";

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    url = normalizeUrl(url);
    if (url.isEmpty()) {
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);
#if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(10000);
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32WaterBot/1.0");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(8000);
    http.setTimeout(15000);

    if (!http.begin(client, url)) {
        http.end();
        client.stop();
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        client.stop();
        return false;
    }

    contentOut = http.getString();
    http.end();
    client.stop();

    contentOut.trim();
    return !contentOut.isEmpty();
}

void GitCtrl::setupOTA(String checkUrl, String currentVersion) {
    _otaCheckUrl = checkUrl;
    _currentVersion = currentVersion;
}

String GitCtrl::detectNewVersion(String &firmwareUrlOut) {
    // 读取 version.json：若 version != 当前版本，则认为有更新。
    // firmwareUrlOut：优先使用 version.json 中的 url/bin_url；如果缺失或是 raw 链接，则自动用 Releases 地址。
    if (WiFi.status() != WL_CONNECTED || _otaCheckUrl.isEmpty()) {
        Serial.println("[GitCtrl] Check Skipped: No WiFi or URL");
        return "";
    }

    Serial.println("[GitCtrl] Checking for firmware updates...");

    WiFiClientSecure client;
    client.setInsecure(); // 跳过证书验证
    client.setTimeout(15000);
#if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(10000);
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32WaterBot/1.0");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(8000);
    http.setTimeout(15000);

    if (!http.begin(client, _otaCheckUrl)) {
        Serial.println("[GitCtrl] http.begin failed for version check");
        client.stop();
        return "";
    }
    int httpCode = http.GET();
    String newVersion = "";

    if (httpCode == 200) { // HTTP_CODE_OK
        String payload = http.getString();

        // 兼容 ArduinoJson（以及 Blinker 内置的 ArduinoJson 变体）：使用 DynamicJsonDocument 显式分配容量
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.printf("[GitCtrl] JSON parse error: %s\n", error.c_str());
        } else {
            const char* remoteVersion = doc["version"];
            const char* firmwareUrl = doc["url"];
            if (!firmwareUrl) firmwareUrl = doc["bin_url"]; // 兼容字段名

            if (remoteVersion && String(remoteVersion) != _currentVersion) {
                newVersion = String(remoteVersion);

                String urlCandidate = firmwareUrl ? normalizeUrl(String(firmwareUrl)) : "";
                // version.json 显式指定了 url → 直接使用，不再自作主张切换
                // 仅当未提供 url 时，回退为 Gitee Releases 地址
                if (urlCandidate.isEmpty()) {
                    String otaOwner = _owner;
                    String otaRepo = _repo;
                    parseGiteeOwnerRepoFromUrl(_otaCheckUrl, otaOwner, otaRepo);
                    firmwareUrlOut = buildGiteeReleaseFirmwareUrl(otaOwner, otaRepo, newVersion);
                } else {
                    firmwareUrlOut = urlCandidate;
                }

                Serial.printf("[GitCtrl] New version found: %s\n", remoteVersion);
                Serial.print("[GitCtrl] Firmware URL: ");
                Serial.println(firmwareUrlOut);
            } else {
                Serial.println("[GitCtrl] System is up to date.");
            }
        }
    } else {
        Serial.printf("[GitCtrl] Check Failed, HTTP Code: %d\n", httpCode);
    }
    http.end();
    client.stop();
    return newVersion;
}

bool GitCtrl::performUpdate(String firmwareUrl) {
    if (WiFi.status() != WL_CONNECTED) return false;

    firmwareUrl = normalizeUrl(firmwareUrl);

#if defined(ARDUINO_ARCH_ESP32)
    WiFi.setSleep(false);
#endif

    bool isDirectUrl = (firmwareUrl.indexOf(".cos.") >= 0 && firmwareUrl.indexOf(".myqcloud.com") >= 0);

    // COS: 强制走 HTTP，避免 mbedTLS SSL 缓冲区溢出 (-76)
    if (isDirectUrl && firmwareUrl.startsWith("https://")) {
        firmwareUrl = "http://" + firmwareUrl.substring(8);
    }

    Serial.printf("[GitCtrl] Starting OTA Update (%s)...\n", isDirectUrl ? "direct(HTTP)" : "segmented");
    Serial.println(isDirectUrl ? firmwareUrl : stripQueryParam(firmwareUrl, "access_token"));

    bool ok;
    if (isDirectUrl) {
        ok = otaUpdateDirect(firmwareUrl);
    } else {
        String firmwareUrlNoToken = stripQueryParam(firmwareUrl, "access_token");
        ok = otaUpdateViaStream(firmwareUrlNoToken);
    }

    if (ok) {
        Serial.println("[GitCtrl] OTA OK! Rebooting...");
        delay(1000);
        ESP.restart();
        return true;
    }

    Serial.println("[GitCtrl] OTA failed (will retry on next wakeup)");
    return false;
}

bool GitCtrl::uploadTextFile(String remotePath, String content, String message, String branch, void (*keepAlive)()) {
    if (WiFi.status() != WL_CONNECTED) {
         Serial.println("[GitCtrl] Upload Failed: No WiFi");
         return false;
    }

    // 准备 API URL
    // POST https://gitee.com/api/v5/repos/{owner}/{repo}/contents/{path}
    String apiUrl = "https://gitee.com/api/v5/repos/" + _owner + "/" + _repo + "/contents/" + remotePath;

    String encodedContent = base64Encode(content);
    if (encodedContent.isEmpty()) {
        Serial.println("[GitCtrl] Base64 encode failed");
        return false;
    }

    // 构造 JSON（避免 ArduinoJson 在大 content 场景下的堆碎片/容量不足）
    String jsonBody;
    jsonBody.reserve(_token.length() + encodedContent.length() + message.length() + branch.length() + 128);
    jsonBody += "{\"access_token\":\"";
    jsonBody += jsonEscape(_token);
    jsonBody += "\",\"content\":\"";
    jsonBody += encodedContent; // base64 安全字符集，无需额外转义
    jsonBody += "\",\"message\":\"";
    jsonBody += jsonEscape(message);
    jsonBody += "\",\"branch\":\"";
    jsonBody += jsonEscape(branch);
    jsonBody += "\"}";

    // 发送请求
    // 上传阶段禁用 WiFi 省电，降低 TLS 断流概率（上传完成后不主动恢复，后续会深睡断开）
#if defined(ARDUINO_ARCH_ESP32)
    WiFi.setSleep(false);
#endif

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);
#if defined(ARDUINO_ARCH_ESP32)
    client.setHandshakeTimeout(10000);
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32WaterBot/1.0");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(8000);
    http.setTimeout(15000);

    Serial.print("[GitCtrl] Uploading file to: ");
    Serial.println(remotePath);

    if (!http.begin(client, apiUrl)) {
        Serial.println("[GitCtrl] Upload Failed: http.begin failed");
        http.end();
        client.stop();
        return false;
    }
    http.addHeader("Content-Type", "application/json;charset=UTF-8");

    if (keepAlive) keepAlive();
    int httpCode = http.POST(jsonBody);
    if (keepAlive) keepAlive();

    bool ok = (httpCode == 201);
    if (ok) {
        Serial.println("[GitCtrl] Upload Success!");
    } else {
        Serial.printf("[GitCtrl] Upload Failed. Code: %d\n", httpCode);
        // 错误体可能很大/很慢；超时已限制，但仍只在失败时读取
        Serial.println(http.getString());
    }

    http.end();
    client.stop();
    return ok;
}

String GitCtrl::base64Encode(String input) {
    // Determine output length
    size_t inputLen = input.length();
    size_t outputLen = 4 * ((inputLen + 2) / 3);

    // Allocate buffer (plus null terminator)
    unsigned char* outBuffer = (unsigned char*) malloc(outputLen + 1);
    if (!outBuffer) return "";

    size_t actualLen = 0;
    mbedtls_base64_encode(outBuffer, outputLen + 1, &actualLen, (const unsigned char*)input.c_str(), inputLen);

    outBuffer[actualLen] = '\0'; // Null terminate
    String result = (char*)outBuffer;
    free(outBuffer);

    return result;
}
