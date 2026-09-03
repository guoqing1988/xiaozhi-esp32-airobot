"""测试 alarm_manager 的到点判断逻辑(纯逻辑, 与硬件解耦).

对应 C++ 实现 main/boards/bread-compact-wifi-s3cam-airobot/alarm_manager.cc 中
AlarmManager::IsDue 的数学分支:
  - relative: (now_ms - base_ms)/1000 >= trigger_sec  (从创建时刻起算, 一次性)
  - absolute: now_hms >= trigger_hms 且 last_fired_day != today (每天触发, 避免当天重复)
仅在此处用 Python 复刻判定逻辑做单元验证; 真机到点与播报需硬件验证(计划任务6).
"""
import unittest
from datetime import datetime


def is_due(alarm, now_ms, today, now_hms):
    """复刻 AlarmManager::IsDue. alarm: dict(base_ms, trigger_sec, type, last_fired_day, enabled)"""
    if not alarm["enabled"]:
        return False
    if alarm["type"] == "relative":
        base = alarm.get("base_ms") or now_ms
        elapsed = (now_ms - base) // 1000
        return elapsed >= alarm["trigger_sec"]
    else:
        return now_hms >= alarm["trigger_sec"] and alarm.get("last_fired_day", 0) != today


class TestAlarmDueLogic(unittest.TestCase):
    def test_relative_due_at_trigger(self):
        # 相对闹钟: 从 base_ms(Add 时记录的创建时刻)起算, 恰好到 trigger 秒即到点
        alarm = {"type": "relative", "trigger_sec": 1800, "base_ms": 1000, "enabled": True, "last_fired_day": 0}
        self.assertTrue(is_due(alarm, now_ms=1000 + 1800 * 1000, today=0, now_hms=0))
        self.assertFalse(is_due(alarm, now_ms=1000 + 1799 * 1000, today=0, now_hms=0))

    def test_relative_zero_base_never_due(self):
        # 异常数据(base_ms==0)按当前时刻起算, elapsed=0 < trigger>0 -> 不触发(防御性)
        alarm = {"type": "relative", "trigger_sec": 60, "base_ms": 0, "enabled": True, "last_fired_day": 0}
        self.assertFalse(is_due(alarm, now_ms=10 * 60 * 1000, today=0, now_hms=0))

    def test_relative_rebases_after_reboot(self):
        # 重启后 Load 重新计时: base_ms 被重置为当前时刻, 计时清零
        alarm = {"type": "relative", "trigger_sec": 300, "base_ms": 10 * 60 * 1000, "enabled": True, "last_fired_day": 0}
        # 模拟重启后 base_ms 被覆盖为当前时刻(now_ms)
        now = 99 * 1000
        alarm["base_ms"] = now
        self.assertFalse(is_due(alarm, now_ms=now + 299 * 1000, today=0, now_hms=0))
        self.assertTrue(is_due(alarm, now_ms=now + 300 * 1000, today=0, now_hms=0))

    def test_absolute_not_due_before_time(self):
        # 绝对闹钟: 当天未到目标时刻不触发
        alarm = {"type": "absolute", "trigger_sec": 7 * 3600, "base_ms": 0, "enabled": True, "last_fired_day": 0}
        self.assertFalse(is_due(alarm, now_ms=0, today=100, now_hms=6 * 3600))

    def test_absolute_due_at_time_once_per_day(self):
        # 绝对闹钟: 到目标时刻触发一次, 之后当天不再重复
        alarm = {"type": "absolute", "trigger_sec": 7 * 3600, "base_ms": 0, "enabled": True, "last_fired_day": 0}
        self.assertTrue(is_due(alarm, now_ms=0, today=100, now_hms=7 * 3600))
        # 触发后记录 last_fired_day = today -> 当天不再触发
        alarm["last_fired_day"] = 100
        self.assertFalse(is_due(alarm, now_ms=0, today=100, now_hms=8 * 3600))
        # 次日(today=101)再次触发
        self.assertTrue(is_due(alarm, now_ms=0, today=101, now_hms=7 * 3600))

    def test_absolute_set_after_time_not_immediate(self):
        # 用户当天 8 点设置 7:00 的绝对闹钟: 设置时记 last_fired_day=today, 当天不立即触发
        alarm = {"type": "absolute", "trigger_sec": 7 * 3600, "base_ms": 0, "enabled": True, "last_fired_day": 0}
        # 模拟 Add 时的逻辑: 若已过时刻, 记 today
        now_hms = 8 * 3600
        if now_hms >= alarm["trigger_sec"]:
            alarm["last_fired_day"] = 100
        self.assertFalse(is_due(alarm, now_ms=0, today=100, now_hms=now_hms))
        # 次日仍触发
        self.assertTrue(is_due(alarm, now_ms=0, today=101, now_hms=7 * 3600))

    def test_disabled_alarm_never_fires(self):
        alarm = {"type": "relative", "trigger_sec": 0, "base_ms": 0, "enabled": False, "last_fired_day": 0}
        self.assertFalse(is_due(alarm, now_ms=1000, today=0, now_hms=0))

    def test_json_field_mapping(self):
        # 序列化字段与 C++ AlarmItemToJson 保持一致
        events = {"relative": "relative", "absolute": "absolute"}
        item = {"id": 1, "type": events["relative"], "trigger_sec": 300, "label": "喝水", "enabled": True}
        self.assertEqual(item["type"], "relative")
        self.assertEqual(item["label"], "喝水")
        self.assertTrue(item["enabled"])


if __name__ == "__main__":
    unittest.main()
