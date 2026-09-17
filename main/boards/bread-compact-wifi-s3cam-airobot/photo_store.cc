#include "photo_store.h"

#include <cJSON.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "settings.h"

namespace {

// 两个目录分开存：网页按钮拍的（用户主动留档）与 AI 拍照留档（可能很频繁）。
constexpr const char *kWebDir = "/sdcard/photos";
constexpr const char *kAiDir = "/sdcard/photos_ai";
// 每目录保留上限，超出按拍摄时间删最旧的。VGA JPEG 约 30~60KB，100 张 ≈ 5MB，对 TF 卡可忽略。
constexpr int kMaxPhotos = 100;
// NVS：AI 拍照是否存卡（默认开）
constexpr const char *kAiSaveNs = "photo";
constexpr const char *kAiSaveKey = "ai_save";
// 路径缓冲：要装得下 dir(最长 17) + '/' + 文件名。
// 用 320 而不是刚好够：目录项名字(d_name)最长可达 255，缓冲过小会触发
// -Werror=format-truncation（对无关的超长文件名也要能安全跳过）。
constexpr size_t kPathMax = 320;
constexpr size_t kNameMax = 64;
// 2020-01-01：系统时间早于它说明 RTC 还没同步（联网后才有正确时间）
constexpr long long kTimeSyncedThreshold = 1577836800LL;

const char *DirOf(PhotoKind kind) { return (kind == PhotoKind::kAi) ? kAiDir : kWebDir; }

const char *KindName(PhotoKind kind) { return (kind == PhotoKind::kAi) ? "ai" : "web"; }

// 文件名后缀判断（不区分大小写，与 http_upload_server.cc 的 HasSuffix 同义）
bool HasJpgSuffix(const char *name) {
    const size_t nl = strlen(name);
    return nl >= 4 && strcasecmp(name + nl - 4, ".jpg") == 0;
}

// 照片文件名净化：照片名只可能是本模块自己生成的（时间戳或 P_0001.jpg），
// 所以这里用**拒绝**而不是替换——任何含路径分隔符 / ".." / 非 .jpg 的名字都判非法，
// 避免请求被构造成 "../../config.json" 之类读走或删掉卡上其它文件。
bool IsSafePhotoName(const char *name) {
    if (name == nullptr || name[0] == '\0' || strlen(name) >= kNameMax) {
        return false;
    }
    if (strstr(name, "..") != nullptr) {
        return false;
    }
    for (const char *p = name; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            return false;
        }
    }
    return HasJpgSuffix(name);
}

// 目录存在则返回 true；不存在则创建。
bool EnsureDir(const char *dir) {
    struct stat st = {};
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(dir, 0775) == 0;
}

struct PhotoEntry {
    std::string name;
    long long mtime;
    long size;
};

// 扫描目录里的 .jpg（名称/大小/拍摄时间）。目录不存在时返回空表。
std::vector<PhotoEntry> CollectEntries(const char *dir) {
    std::vector<PhotoEntry> items;
    DIR *d = opendir(dir);
    if (d == nullptr) {
        return items;
    }
    struct dirent *e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_type != DT_REG) {
            continue;
        }
        const char *name = e->d_name;
        if (!HasJpgSuffix(name)) {
            continue;  // 只认照片，目录里的其它文件不参与列表/清理
        }
        char path[kPathMax];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        struct stat st = {};
        if (stat(path, &st) != 0) {
            continue;
        }
        items.push_back({name, static_cast<long long>(st.st_mtime), static_cast<long>(st.st_size)});
    }
    closedir(d);
    return items;
}

// 新的在前：先按拍摄时间倒序，同一秒内（未同步时 mtime 全相同）再按文件名倒序。
// 文件名用时间戳或递增序号，字典序与拍摄先后一致，所以这个次序是稳定的。
void SortNewestFirst(std::vector<PhotoEntry> &items) {
    std::sort(items.begin(), items.end(), [](const PhotoEntry &a, const PhotoEntry &b) {
        if (a.mtime != b.mtime) {
            return a.mtime > b.mtime;
        }
        return a.name > b.name;
    });
}

// 系统时间是否已同步（联网 NTP 后才有正确时间）。未同步时不能用时间戳做文件名，
// 否则同一秒内拍的多张会互相覆盖、且文件名全是 1970。
bool TimeSynced() { return static_cast<long long>(time(nullptr)) > kTimeSyncedThreshold; }

// 扫描 "P_%04d.jpg" 形式的最大编号，返回下一个可用编号（未同步时的兜底命名）。
int NextSequence(const char *dir) {
    int max_seq = 0;
    for (const PhotoEntry &it : CollectEntries(dir)) {
        int seq = 0;
        if (sscanf(it.name.c_str(), "P_%d.jpg", &seq) == 1 && seq > max_seq) {
            max_seq = seq;
        }
    }
    return max_seq + 1;
}

// 生成文件名：时间已同步用时间戳（一眼看出拍摄时间、天然按时间排序），
// 未同步则用递增序号兜底（绝不覆盖已有照片）。
// 同一秒连拍会产生同名，这里探测到已存在就追加 _1/_2… 后缀，确保不覆盖。
void MakeFileName(const char *dir, char *out, size_t out_size) {
    // base 只需装下时间戳(15 字符)或 P_9999：刻意留小，被 snprintf 检查截断时不会报警
    char base[40];
    if (TimeSynced()) {
        const time_t now = time(nullptr);
        struct tm t = {};
        localtime_r(&now, &t);
        snprintf(base, sizeof(base), "%04d%02d%02d_%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        snprintf(base, sizeof(base), "P_%04d", NextSequence(dir));
    }

    snprintf(out, out_size, "%s.jpg", base);
    char path[kPathMax];
    for (int dup = 1; dup < 100; ++dup) {
        snprintf(path, sizeof(path), "%s/%s", dir, out);
        struct stat st = {};
        if (stat(path, &st) != 0) {
            return;  // 该名字还没被占用
        }
        snprintf(out, out_size, "%s_%d.jpg", base, dup);
    }
}

// 写卡成功后的滚动清理：超出 kMaxPhotos 的部分按最旧的先删。
// ⚠ 只能在上传/写入**成功之后**调用——写失败还来清理就会白删用户的旧照片。
void TrimDir(const char *dir) {
    std::vector<PhotoEntry> items = CollectEntries(dir);
    if (static_cast<int>(items.size()) <= kMaxPhotos) {
        return;
    }
    std::sort(items.begin(), items.end(),
              [](const PhotoEntry &a, const PhotoEntry &b) { return a.mtime < b.mtime; });
    const size_t remove_count = items.size() - static_cast<size_t>(kMaxPhotos);
    for (size_t i = 0; i < remove_count; ++i) {
        char path[kPathMax];
        snprintf(path, sizeof(path), "%s/%s", dir, items[i].name.c_str());
        remove(path);  // 删不掉（如卡只读）只影响数量上限，不影响本次拍照结果
    }
}

// 文件系统操作互斥：写卡可能来自两个线程——httpd 任务（网页拍照）与
// Explain() 的编码线程（AI 拍照留档，见 esp32_camera 的 JPEG 观察者）。
// FATFS 自身也有锁，但清理要"扫描目录 + 批量删除"，两个线程交错会互相删到刚列出的项，
// 所以在更高层再串行一次。
std::mutex s_store_mtx;

}  // namespace

bool PhotoStoreInit() {
    // 两个目录任一创建失败都认为相册不可用（网页会隐藏相册入口，拍照仍走 PSRAM）
    const bool web_ok = EnsureDir(kWebDir);
    const bool ai_ok = EnsureDir(kAiDir);
    return web_ok && ai_ok;
}

bool PhotoStoreSave(PhotoKind kind, const uint8_t *jpeg, size_t len, std::string *out_name) {
    if (out_name != nullptr) {
        out_name->clear();
    }
    if (jpeg == nullptr || len == 0) {
        return false;
    }
    const char *dir = DirOf(kind);

    // 目录创建也要在锁内：两个线程同时 mkdir 时其中一个会因 EEXIST 失败，
    // 进而让这次写卡白白失败。
    std::lock_guard<std::mutex> lock(s_store_mtx);
    if (!EnsureDir(dir)) {
        return false;
    }
    char name[kNameMax];
    MakeFileName(dir, name, sizeof(name));
    char path[kPathMax];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const size_t written = fwrite(jpeg, 1, len, f);
    // 先关闭再判断：写完不关闭的话，stat/清理可能看到长度不完整的文件
    const int closed = fclose(f);
    if (written != len || closed != 0) {
        remove(path);  // 半截文件比没有更糟，直接删掉
        return false;
    }

    TrimDir(dir);
    if (out_name != nullptr) {
        *out_name = name;
    }
    return true;
}

std::string PhotoStoreListJson(PhotoKind kind) {
    std::lock_guard<std::mutex> lock(s_store_mtx);
    std::vector<PhotoEntry> items = CollectEntries(DirOf(kind));
    SortNewestFirst(items);

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{\"ok\":false,\"error\":\"no memory\"}";
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "kind", KindName(kind));
    // 把 AI 存卡开关一并带上：前端拉列表时就能初始化那个勾选框，省一次请求（也省一次 CORS 预检）
    cJSON_AddBoolToObject(root, "ai_save", PhotoStoreGetAiSave());
    cJSON_AddNumberToObject(root, "count", static_cast<double>(items.size()));
    cJSON_AddNumberToObject(root, "limit", kMaxPhotos);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    if (arr == nullptr) {
        cJSON_Delete(root);
        return "{\"ok\":false,\"error\":\"no memory\"}";
    }
    for (const PhotoEntry &it : items) {
        cJSON *item = cJSON_CreateObject();
        if (item == nullptr) {
            break;
        }
        cJSON_AddStringToObject(item, "name", it.name.c_str());
        cJSON_AddNumberToObject(item, "size", static_cast<double>(it.size));
        cJSON_AddNumberToObject(item, "mtime", static_cast<double>(it.mtime));
        cJSON_AddItemToArray(arr, item);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string body = (out != nullptr) ? out : "{\"ok\":false,\"error\":\"encode failed\"}";
    free(out);
    return body;
}

bool PhotoStoreDelete(PhotoKind kind, const char *name) {
    if (!IsSafePhotoName(name)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_store_mtx);
    char path[kPathMax];
    snprintf(path, sizeof(path), "%s/%s", DirOf(kind), name);
    return remove(path) == 0;
}

bool PhotoStoreResolvePath(PhotoKind kind, const char *name, char *out, size_t out_size) {
    if (!IsSafePhotoName(name) || out == nullptr || out_size == 0) {
        return false;
    }
    // name 已限制长度（< kNameMax），不会截断出不完整的路径
    snprintf(out, out_size, "%s/%s", DirOf(kind), name);
    return true;
}

int PhotoStoreClear(PhotoKind kind) {
    std::lock_guard<std::mutex> lock(s_store_mtx);
    const char *dir = DirOf(kind);
    int removed = 0;
    for (const PhotoEntry &it : CollectEntries(dir)) {
        char path[kPathMax];
        snprintf(path, sizeof(path), "%s/%s", dir, it.name.c_str());
        if (remove(path) == 0) {
            removed++;
        }
    }
    return removed;
}

int PhotoStoreCount(PhotoKind kind) {
    std::lock_guard<std::mutex> lock(s_store_mtx);
    return static_cast<int>(CollectEntries(DirOf(kind)).size());
}

void PhotoStoreSetAiSave(bool on) {
    Settings settings(kAiSaveNs, true);
    settings.SetBool(kAiSaveKey, on);
}

bool PhotoStoreGetAiSave() {
    Settings settings(kAiSaveNs, false);
    return settings.GetBool(kAiSaveKey, true);  // 默认开：AI 拍的照片默认也留档
}
