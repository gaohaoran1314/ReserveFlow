// =============================================================================
// src/SlotUtil.h  —  时间 ↔ 半小时槽位换算内核（header-only，纯函数，零依赖）
// 对应 ARCH-P1 §3.1 / D-P1-8
//
// 口径（D-P1-8）：
//   同一天内、起止必须半点对齐（:00 / :30）、秒为 0、start < end、
//   不得早于当前时刻、跨度 <= 8 小时（16 槽）。跨天预约 P1 不支持。
//
// 设计约束：
//   1) 只依赖标准库，可被任何层包含（不含 mysql.h / json.hpp / 日志）；
//   2) 所有函数无副作用、不抛异常，失败一律通过返回值或 SpanResult::err 表达；
//   3) 错误文案直接作为 3009 的 msg 使用（§7「3009 的 msg 拼接规则」），
//      故本文件内文案必须与文档逐字一致，前端会原样展示。
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rf::slot {

constexpr int kSlotMinutes  = 30;   // 每槽 30 分钟
constexpr int kSlotsPerDay  = 48;   // 0..47
constexpr int kMaxSpanSlots = 16;   // 最长 8 小时

/** 拆解后的时间点（本地时间，无时区） */
struct DateTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

namespace detail {

/** ASCII 数字判断（避免依赖 <cctype> 的本地化行为） */
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

/**
 * 从 s 的 pos 起取定长 n 位的十进制数。
 * @return 越界或含非数字时返回 false
 */
inline bool takeInt(const std::string& s, size_t pos, size_t n, int& out) {
    if (pos + n > s.size()) {
        return false;
    }
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (!isDigit(c)) {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

/** 闰年判断（公历规则） */
inline bool isLeapYear(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/** 某年某月的天数；month 越界返回 0 */
inline int daysInMonth(int y, int m) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) {
        return 0;
    }
    if (m == 2 && isLeapYear(y)) {
        return 29;
    }
    return kDays[m - 1];
}

/** 日期字段合法性校验（含闰年 2 月 29） */
inline bool validYmd(int y, int m, int d) {
    if (y < 1970 || y > 9999) {
        return false;
    }
    if (m < 1 || m > 12) {
        return false;
    }
    return d >= 1 && d <= daysInMonth(y, m);
}

/**
 * 两个时间点的字典序比较（年月日时分秒逐级比较，与 epoch 顺序一致）。
 * 不用 mktime：避免夏令时/时区对「同一天内比较」产生额外语义。
 * @return a < b → -1；a == b → 0；a > b → 1
 */
inline int compare(const DateTime& a, const DateTime& b) {
    if (a.year != b.year)       return a.year < b.year ? -1 : 1;
    if (a.month != b.month)     return a.month < b.month ? -1 : 1;
    if (a.day != b.day)         return a.day < b.day ? -1 : 1;
    if (a.hour != b.hour)       return a.hour < b.hour ? -1 : 1;
    if (a.minute != b.minute)   return a.minute < b.minute ? -1 : 1;
    if (a.second != b.second)   return a.second < b.second ? -1 : 1;
    return 0;
}

/** "yyyy-MM-dd" */
inline std::string formatDate(const DateTime& t) {
    char buf[16] = {0};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.year, t.month, t.day);
    return std::string(buf);
}

}  // namespace detail

/**
 * 解析 "yyyy-MM-ddTHH:mm:ss"。也接受空格分隔（"yyyy-MM-dd HH:mm:ss"）。
 * 只读取前 19 个字符，容忍末尾 'Z' 或毫秒等后缀。
 * @param s   待解析字符串
 * @param out 成功时写入
 * @return 格式非法返回 false
 */
inline bool parseIso(const std::string& s, DateTime& out) {
    if (s.size() < 19) {
        return false;
    }
    if (s[4] != '-' || s[7] != '-' || s[13] != ':' || s[16] != ':') {
        return false;
    }
    if (s[10] != 'T' && s[10] != ' ' && s[10] != 't') {
        return false;
    }
    DateTime t;
    if (!detail::takeInt(s, 0, 4, t.year) || !detail::takeInt(s, 5, 2, t.month) ||
        !detail::takeInt(s, 8, 2, t.day) || !detail::takeInt(s, 11, 2, t.hour) ||
        !detail::takeInt(s, 14, 2, t.minute) || !detail::takeInt(s, 17, 2, t.second)) {
        return false;
    }
    if (!detail::validYmd(t.year, t.month, t.day)) {
        return false;
    }
    if (t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 || t.second < 0 ||
        t.second > 59) {
        return false;
    }
    out = t;
    return true;
}

/** 解析 "yyyy-MM-dd"（时分秒置 0） */
inline bool parseDate(const std::string& s, DateTime& out) {
    if (s.size() < 10) {
        return false;
    }
    if (s[4] != '-' || s[7] != '-') {
        return false;
    }
    DateTime t;
    if (!detail::takeInt(s, 0, 4, t.year) || !detail::takeInt(s, 5, 2, t.month) ||
        !detail::takeInt(s, 8, 2, t.day)) {
        return false;
    }
    if (!detail::validYmd(t.year, t.month, t.day)) {
        return false;
    }
    out = t;
    return true;
}

/**
 * 时钟点 → 槽位序号。
 * @return 0..47；非半点对齐（分不是 0 或 30）或秒非 0 返回 -1
 */
inline int indexOf(const DateTime& t) {
    if (t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59) {
        return -1;
    }
    if (t.second != 0) {
        return -1;
    }
    if (t.minute == 0) {
        return t.hour * 2;
    }
    if (t.minute == 30) {
        return t.hour * 2 + 1;
    }
    return -1;   // 非半点对齐
}

/**
 * 反向：槽位序号 → "HH:mm"，用于前端渲染。
 * @param index 0..47；额外支持 48 → "24:00"（渲染最后一槽的右端点用）
 * @return 越界返回空串
 */
inline std::string slotLabel(int index) {
    if (index < 0 || index > kSlotsPerDay) {
        return std::string();
    }
    const int hour   = index / 2;
    const int minute = (index % 2) * kSlotMinutes;
    char buf[8] = {0};
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return std::string(buf);
}

/** 区间校验 + 槽位展开结果 */
struct SpanResult {
    bool             ok = false;
    std::string      err;                ///< 可直接作为 3009 的 msg（已含中文原因）
    std::string      date;               ///< "yyyy-MM-dd"
    int              startIndex = 0;     ///< 含
    int              endIndex   = 0;     ///< 不含
    std::vector<int> indexes;            ///< [startIndex, endIndex) 全部槽
};

/**
 * 校验预约区间并展开槽位（D-P1-8）。
 * 规则：同一天、半点对齐、秒为 0、start < end、start 不得早于 now、跨度 <= kMaxSpanSlots。
 *
 * 校验顺序刻意固定（先格式、再秒、再跨天、再对齐、再顺序、再跨度、最后过去时间），
 * 保证同一请求每次得到同一条文案，前端提示不会抖动。
 *
 * @param startTime 请求开始时间 "yyyy-MM-ddTHH:mm:ss"
 * @param endTime   请求结束时间
 * @param nowIso    当前时间（注入以便单测；生产传 TimeUtil::nowString()）。
 *                  传空串表示跳过「过去时间」校验（仅供离线推演/单测）。
 */
inline SpanResult makeSpan(const std::string& startTime, const std::string& endTime,
                           const std::string& nowIso) {
    SpanResult out;

    DateTime start;
    DateTime end;
    if (!parseIso(startTime, start) || !parseIso(endTime, end)) {
        out.err = "预约时间不合法：时间格式应为 yyyy-MM-ddTHH:mm:ss";
        return out;
    }
    if (start.second != 0 || end.second != 0) {
        out.err = "预约时间不合法：秒必须为 0";
        return out;
    }
    if (start.year != end.year || start.month != end.month || start.day != end.day) {
        out.err = "预约时间不合法：P1 不支持跨天预约";
        return out;
    }

    const int startIndex = indexOf(start);
    const int endIndex   = indexOf(end);
    if (startIndex < 0 || endIndex < 0) {
        out.err = "预约时间不合法：起止时间必须落在整点或半点";
        return out;
    }
    if (startIndex >= endIndex) {
        out.err = "预约时间不合法：结束时间必须晚于开始时间";
        return out;
    }
    if (endIndex - startIndex > kMaxSpanSlots) {
        out.err = "预约时间不合法：单次预约不得超过 8 小时";
        return out;
    }

    DateTime now;
    if (!nowIso.empty() && parseIso(nowIso, now)) {
        // 不得早于当前时刻：start 已是半点对齐，now 带秒，故用字典序比较即可
        if (detail::compare(start, now) < 0 || detail::compare(end, now) <= 0) {
            out.err = "预约时间不合法：不能预约已过去的时段";
            return out;
        }
    }

    out.ok         = true;
    out.date       = detail::formatDate(start);
    out.startIndex = startIndex;
    out.endIndex   = endIndex;
    out.indexes.reserve(static_cast<size_t>(endIndex - startIndex));
    for (int i = startIndex; i < endIndex; ++i) {
        out.indexes.push_back(i);
    }
    return out;
}

}  // namespace rf::slot
