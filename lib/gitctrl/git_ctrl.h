#ifndef GIT_CTRL_H
#define GIT_CTRL_H

#include <Arduino.h>

class GitCtrl {
public:
    /**
     * @brief 构造一个新的 Git 控制对象
     *
     * @param token Gitee 个人访问令牌（需要 'projects' 权限）
     * @param owner Gitee 用户名或组织名称
     * @param repo 用于文件上传的目标仓库名称
     */
    GitCtrl(String token, String owner, String repo);

    /**
     * @brief 设置 OTA 配置
     *
     * @param checkUrl 版本信息的原始 URL（例如 "https://gitee.com/.../raw/master/version.json"）
     * @param currentVersion 当前固件版本（例如 "1.0.0"）
     */
    void setupOTA(String checkUrl, String currentVersion);

    /**
     * @brief 检测远程是否有新版本可用
     *
     * @param firmwareUrlOut 引用一个 String 用于存储固件下载 URL（如果检测到更新）
      * 说明：version.json 可仅包含 "version" 字段；当 "url"/"bin_url" 缺失或指向 raw 链接时，
      *      将自动使用 Gitee Releases 链接：
      *      https://gitee.com/{owner}/{repo}/releases/download/v{version}/firmware.bin
     * @return String 如果找到新版本则返回新版本号，否则返回空字符串 ""
     */
    String detectNewVersion(String &firmwareUrlOut);

    /**
     * @brief 使用提供的URL执行OTA更新
     * 成功时会阻塞并重启设备。
     *
     * @param firmwareUrl .bin文件的URL
     * @return bool 如果更新成功返回True（设备将重启），失败返回False。
     */
    bool performUpdate(String firmwareUrl);

    /**
     * @brief 将一个小文本文件上传到 Gitee 仓库
     * 注意：此方法使用的是创建文件 API，如果文件已存在，可能会失败（更新需要 SHA）。
     * 建议使用唯一的文件名，例如 "logs/log_123.txt"。
     *
     * @param remotePath 仓库中的路径（例如 "data/log.txt"）
     * @param content 要上传的字符串内容
     * @param message 提交信息
     * @param branch 分支名称（默认 "master"）
     * @return 成功返回 true
     * @return 失败返回 false
     */
    bool uploadTextFile(String remotePath, String content, String message = "Uploaded from ESP32",
                         String branch = "master", void (*keepAlive)() = nullptr);

    /**
     * @brief 下载一个小文本/JSON 文件（HTTPS GET + 自动跟随重定向）
     *
     * 用途：远程参数下发（例如 setting_online.json）。建议文件保持较小以提升成功率。
     *
     * @param url 完整 URL
     * @param contentOut 返回内容
     * @return 成功返回 true
     */
    bool downloadText(String url, String &contentOut);

private:
    String _token;
    String _owner;
    String _repo;

    String _otaCheckUrl;
    String _currentVersion;

    // Helper for Base64 encoding
    String base64Encode(String input);
};

#endif
