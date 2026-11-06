# Grafana / Prometheus 查询语句

- 实时 A 计权估算分贝：`sound_level_dba{job="sound-meter"}`
- 实时未计权估算分贝：`sound_level_db{job="sound-meter"}`
- 最近一小时分贝范围最大值：`max_over_time(sound_level_dba{job="sound-meter"}[1h])`
- 最近一天分贝最大值：`max_over_time(sound_level_dba{job="sound-meter"}[24h])`
- 最近一小时算术均值：`avg_over_time(sound_level_dba{job="sound-meter"}[1h])`
- 最近一小时能量平均（声压级的近似物理均值）：
  `10 * log10(avg_over_time((10 ^ (sound_level_dba{job="sound-meter"} / 10))[1h:]))`
- 分贝仪在线：`up{job="sound-meter"}`；麦克风读数新鲜：`sound_meter_audio_up{job="sound-meter"}`。

注意：`avg_over_time` 是 dB 数值的算术均值，不等于能量平均声级 LAeq。读数未经专业声学校准，不能直接用于法定噪声合规判断。
