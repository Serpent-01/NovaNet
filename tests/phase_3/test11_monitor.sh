#!/bin/bash

# ==========================================================
# NovaNet 性能监控脚本 (Soak Test Monitor)
# ==========================================================

# 1. 配置监控目标
TARGET_NAME="multireactor_echo_server"
LOG_FILE="stats.csv"
DURATION=600  # 监控时长（秒）

# 2. 抓取 PID
PID=$(pidof $TARGET_NAME)

if [ -z "$PID" ]; then
    echo -e "\033[31m[Error]\033[0m 找不到进程 $TARGET_NAME，请先启动服务器！"
    exit 1
fi

# 3. 初始化日志文件
echo "Time(s),RSS(KB),FD_Count,CPU(%)" > $LOG_FILE

echo -e "\033[32m[Start]\033[0m 开始监控 PID: $PID ($TARGET_NAME)"
echo -e "[Info]  监控数据将实时保存至 $LOG_FILE"
echo "----------------------------------------------------------"
printf "%-10s %-12s %-10s %-10s\n" "Elapsed" "RSS(KB)" "FD" "CPU(%)"

START_TIME=$(date +%s)

# 4. 循环采样
for ((i=1; i<=$DURATION; i++))
do
    # 检查进程是否还在跑，防止中途崩溃
    if ! kill -0 $PID 2>/dev/null; then
        echo -e "\n\033[31m[Alert]\033[0m 进程 $PID 已意外停止！监控提前结束。"
        break
    fi

    CURRENT_TIME=$(($(date +%s) - START_TIME))
    
    # 采集指标
    RSS=$(ps -o rss= -p $PID | tr -d ' ')
    FD_COUNT=$(ls /proc/$PID/fd | wc -l)
    # 使用 top 采集 CPU，-b 为批处理模式
    CPU=$(top -b -n 1 -p $PID | grep $PID | awk '{print $9}')

    # 写入 CSV
    echo "$CURRENT_TIME,$RSS,$FD_COUNT,$CPU" >> $LOG_FILE

    # 实时打印到终端 (格式化输出)
    printf "\r%-10s %-12s %-10s %-10s" "${CURRENT_TIME}s" "${RSS}" "${FD_COUNT}" "${CPU}%"
    
    sleep 1
done

echo -e "\n----------------------------------------------------------"
echo -e "\033[32m[Done]\033[0m 监控完成！数据已入库 $LOG_FILE。"