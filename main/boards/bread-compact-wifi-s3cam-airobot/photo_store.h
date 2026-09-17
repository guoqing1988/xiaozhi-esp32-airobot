#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// TF 卡照片仓库（板级私有）。
//
// 只管"卡上的照片"这一件事：目录、命名、写入、列表、删除、滚动清理、AI 存卡开关。
// 不碰相机、不碰 HTTP；所有失败一律用返回值上报（本板日志与 Arduino 指令共用 UART0，
// 不允许在拍照/写卡路径上打 ESP_LOG）。
//
// 目录分工（见设计规格 docs/superpowers/specs/2026-09-17-airobot-tfcard-photo-album-design.md）：
//   /sdcard/photos     ← 网页按钮拍照（用户主动留档）
//   /sdcard/photos_ai  ← AI 拍照留档（可开关，默认开）

// 照片来源：决定落到哪个目录
enum class PhotoKind { kWeb, kAi };

// 开机调用一次：确保两个目录存在。失败返回 false（相册不可用，其它功能照常）。
bool PhotoStoreInit();

// 把整块 JPEG 存进对应目录，并在**写成功后**滚动清理（每目录最多 kMaxPhotos 张）。
// out_name 非空时回填实际使用的文件名（供前端提示"已存卡：xxx.jpg"）。
// 失败返回 false（调用方据此提示"仅显示，未存卡"；拍照本身仍算成功）。
bool PhotoStoreSave(PhotoKind kind, const uint8_t *jpeg, size_t len, std::string *out_name = nullptr);

// 列表响应 JSON（按拍摄时间倒序）：
//   {"ok":true,"kind":"web","count":37,"limit":100,"items":[{"name":..,"size":..,"mtime":..}]}
// 目录不存在时返回空列表（不是错误）。
std::string PhotoStoreListJson(PhotoKind kind);

// 删除单张。文件名非法（含路径分隔符/".."/非 .jpg）直接返回 false，防路径穿越。
bool PhotoStoreDelete(PhotoKind kind, const char *name);

// 把 (kind, name) 解析成安全绝对路径（读图用）。名字非法时返回 false。
// 净化判定只在本模块一处实现：HTTP 读/删两条路径共用，避免两边规则走偏。
bool PhotoStoreResolvePath(PhotoKind kind, const char *name, char *out, size_t out_size);

// 清空某目录（只删 .jpg，不动目录内其它文件）；返回删除成功的张数。
int PhotoStoreClear(PhotoKind kind);

// 目录内当前张数（供前端显示 "n/100"）。
int PhotoStoreCount(PhotoKind kind);

// AI 拍照存卡开关（NVS 持久化，键 photo/ai_save，默认开）。
void PhotoStoreSetAiSave(bool on);
bool PhotoStoreGetAiSave();
